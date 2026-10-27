// SPDX-License-Identifier: GPL-2.0-or-later
/* Re-serialize HEVC VPS/SPS/PPS NAL units from VAPictureParameterBufferHEVC
 * so a whole-bitstream stateful decoder (iris) can be fed reconstructed
 * access units.
 *
 * Field order follows ITU-T H.265 and was cross-checked against ffmpeg's
 * trace_headers output for a 1920x1080 x265 stream.  The VA-API picture
 * parameter buffer lacks a few profile_tier_level bits (reserved_zero_44)
 * which are emitted as zero. */

#include <string.h>

#include "hevc_params.h"

struct bs {
	uint8_t *buf;
	size_t size;
	int bits;
	int overflow;
};

static void bs_put(struct bs *b, uint64_t val, int n)
{
	int i;

	for (i = n - 1; i >= 0; i--) {
		int byte = b->bits >> 3;
		int bit = 7 - (b->bits & 7);

		if (byte >= (int)b->size) {
			b->overflow = 1;
			return;
		}
		if (val & (1ULL << i))
			b->buf[byte] |= (1u << bit);
		b->bits++;
	}
}

static void bs_ue(struct bs *b, unsigned int val)
{
	int code = val + 1;
	int n = 32 - __builtin_clz(code);

	bs_put(b, 0, n - 1);
	bs_put(b, code, n);
}

static void bs_se(struct bs *b, int val)
{
	unsigned int code = (val <= 0) ? (unsigned int)(-2 * val)
				      : (unsigned int)(2 * val - 1);

	bs_ue(b, code);
}

static void bs_trailing(struct bs *b)
{
	bs_put(b, 1, 1);
	while (b->bits & 7)
		bs_put(b, 0, 1);
}

static const uint8_t diag4x4_x[16] = {
	0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 1, 2, 3, 2, 3, 3,
};
static const uint8_t diag4x4_y[16] = {
	0, 1, 0, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 3, 2, 3,
};
static const uint8_t diag8x8_x[64] = {
	0, 0, 1, 0, 1, 2, 0, 1, 2, 3, 0, 1, 2, 3, 4, 0,
	1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3,
	4, 5, 6, 7, 1, 2, 3, 4, 5, 6, 7, 2, 3, 4, 5, 6,
	7, 3, 4, 5, 6, 7, 4, 5, 6, 7, 5, 6, 7, 6, 7, 7,
};
static const uint8_t diag8x8_y[64] = {
	0, 1, 0, 2, 1, 0, 3, 2, 1, 0, 4, 3, 2, 1, 0, 5,
	4, 3, 2, 1, 0, 6, 5, 4, 3, 2, 1, 0, 7, 6, 5, 4,
	3, 2, 1, 0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3,
	2, 7, 6, 5, 4, 3, 7, 6, 5, 4, 7, 6, 5, 7, 6, 7,
};

static void
hevc_scaling_list_data(struct bs *b, const VAIQMatrixBufferHEVC *iq)
{
	unsigned int size_id, matrix_id;

	for (size_id = 0; size_id < 4; size_id++) {
		unsigned int step = size_id == 3 ? 3 : 1;

		for (matrix_id = 0; matrix_id < 6; matrix_id += step) {
			const uint8_t *matrix;
			unsigned int count = size_id ? 64 : 16;
			int next = 8;
			unsigned int i;

			bs_put(b, 1, 1); /* scaling_list_pred_mode_flag */
			if (size_id == 0)
				matrix = iq->ScalingList4x4[matrix_id];
			else if (size_id == 1)
				matrix = iq->ScalingList8x8[matrix_id];
			else if (size_id == 2)
				matrix = iq->ScalingList16x16[matrix_id];
			else
				matrix = iq->ScalingList32x32[matrix_id / 3];
			if (size_id == 2) {
				next = iq->ScalingListDC16x16[matrix_id];
				bs_se(b, next - 8);
			} else if (size_id == 3) {
				next = iq->ScalingListDC32x32[matrix_id / 3];
				bs_se(b, next - 8);
			}
			for (i = 0; i < count; i++) {
				unsigned int pos = size_id ?
					diag8x8_y[i] * 8 + diag8x8_x[i] :
					diag4x4_y[i] * 4 + diag4x4_x[i];
				int value = matrix[pos];
				int delta = value - next;

				if (delta > 127)
					delta -= 256;
				else if (delta < -128)
					delta += 256;
				bs_se(b, delta);
				next = value;
			}
		}
	}
}

/* Convert the byte-aligned RBSP held by b into a NAL unit.  The two-byte
 * header is not escaped; emulation-prevention bytes apply to the RBSP only. */
static int hevc_escape_nal(uint8_t *out, size_t out_size, const struct bs *b)
{
	size_t raw_len = (size_t)b->bits >> 3;
	size_t in, out_len = 0;
	int zero_count = 0;

	if (b->overflow || (b->bits & 7) || raw_len < 2 || out_size < 2)
		return -1;
	out[out_len++] = b->buf[0];
	out[out_len++] = b->buf[1];
	for (in = 2; in < raw_len; in++) {
		uint8_t v = b->buf[in];

		if (zero_count >= 2 && v <= 3) {
			if (out_len >= out_size)
				return -1;
			out[out_len++] = 3;
			zero_count = 0;
		}
		if (out_len >= out_size)
			return -1;
		out[out_len++] = v;
		if (v == 0)
			zero_count++;
		else
			zero_count = 0;
	}
	return (int)out_len;
}

/* profile_tier_level( profilePresentFlag, maxNumSubLayersMinus1 ).
 * Emits the general-level fields only (this stream has no sub-layers).
 * Both VPS and SPS carry progressive=1 frame_only=1 level=120 (per ffmpeg
 * authoritative parse of the x265 stream). */
static void
hevc_profile_tier_level(struct bs *b, int profile_present, int max_sublayers,
			int profile_idc, int progressive, int frame_only, int level)
{
	if (profile_present) {
		uint32_t compatibility = profile_idc == 2 ?
			0x20000000U : 0x60000000U;

		bs_put(b, 0, 2);	/* general_profile_space */
		bs_put(b, 0, 1);	/* general_tier_flag */
		bs_put(b, profile_idc, 5); /* Main=1, Main10=2 */
		/* general_profile_compatibility_flag[32].  Real x265 writes
		 * 0x60000000 for Main profile at every level (verified
		 * 1080p/4K/8K); the following 0x90 belongs to the constraint
		 * flags, not to this field. */
		bs_put(b, compatibility, 32);
		bs_put(b, progressive, 1);
		bs_put(b, 0, 1);	/* general_interlaced_source_flag */
		bs_put(b, 0, 1);	/* general_non_packed_constraint_flag */
		bs_put(b, frame_only, 1);
		bs_put(b, 0, 44);	/* general_reserved_zero_44bits (all zero per
				 * ffmpeg parse of the x265 stream) */
		bs_put(b, level, 8);
	}
	for (int i = 0; i < max_sublayers; i++) {
		bs_put(b, 0, 1);	/* sub_layer_profile_present_flag */
		bs_put(b, 0, 1);	/* sub_layer_level_present_flag */
	}
	if (max_sublayers > 0)
		for (int i = max_sublayers; i < 8; i++)
			bs_put(b, 0, 2);	/* reserved_zero_2bits */
	for (int i = 0; i < max_sublayers; i++) {
		bs_put(b, 0, 2);	/* sub_layer_profile_space */
		bs_put(b, 0, 1);	/* sub_layer_tier_flag */
		bs_put(b, 0, 5);	/* sub_layer_profile_idc */
		bs_put(b, 0, 32);	/* sub_layer_compatibility */
		bs_put(b, 0, 1);	/* sub_layer_progressive */
		bs_put(b, 0, 1);	/* sub_layer_interlaced */
		bs_put(b, 0, 1);	/* sub_layer_non_packed */
		bs_put(b, 0, 1);	/* sub_layer_frame_only */
		bs_put(b, 0, 44);	/* sub_layer_reserved_zero_44 */
		bs_put(b, 0, 8);	/* sub_layer_level_idc */
	}
}

static int
hevc_level_for_size(const VAPictureParameterBufferHEVC *pic)
{
	uint64_t samples = (uint64_t)pic->pic_width_in_luma_samples *
			   pic->pic_height_in_luma_samples;

	if (samples <= 921600)
		return 93;
	if (samples <= 2073600)
		return 120;
	if (samples <= 4177920)
		return 153;
	if (samples <= 8355840)
		return 156;
	return 186;
}

static unsigned int
hevc_reorder_pics(const VAPictureParameterBufferHEVC *pic)
{
	/* VA-API does not expose sps_max_num_reorder_pics.  Keep it bounded by
	 * the advertised DPB size; two is sufficient for the common hierarchical
	 * B-frame streams accepted by the Iris firmware. */
	return pic->sps_max_dec_pic_buffering_minus1 < 2 ?
		pic->sps_max_dec_pic_buffering_minus1 : 2;
}

int
hevc_build_vps(uint8_t *out, size_t out_size,
	       const VAPictureParameterBufferHEVC *pic)
{
	struct bs b;
	uint8_t rbsp[512];

	if (!out || !pic)
		return -1;

	memset(rbsp, 0, sizeof(rbsp));
	b.buf = rbsp;
	b.size = sizeof(rbsp);
	b.bits = 0;
	b.overflow = 0;

	bs_put(&b, 0x40, 8);	/* NAL header byte 1: type=32 (VPS) */
	bs_put(&b, 0x01, 8);	/* NAL header byte 2: layer_id=0, tid_plus1=1 */
	bs_put(&b, 0, 4);	/* vps_video_parameter_set_id */
	bs_put(&b, 1, 1);	/* vps_base_layer_internal_flag */
	bs_put(&b, 1, 1);	/* vps_base_layer_available_flag */
	bs_put(&b, 0, 6);	/* vps_max_layers_minus1 */
	bs_put(&b, 0, 3);	/* vps_max_sub_layers_minus1 */
	bs_put(&b, 1, 1);	/* vps_temporal_id_nesting_flag */
	bs_put(&b, 0xffff, 16);	/* vps_reserved_0xffff_16bits */
	hevc_profile_tier_level(&b, 1, 0,
			       pic->bit_depth_luma_minus8 ? 2 : 1, 1,
			       1,
			       hevc_level_for_size(pic));
	bs_put(&b, 1, 1);	/* vps_sub_layer_ordering_info_present */
	bs_ue(&b, pic->sps_max_dec_pic_buffering_minus1);
	bs_ue(&b, hevc_reorder_pics(pic));
	bs_ue(&b, 0);
	bs_put(&b, 0, 6);	/* vps_max_layer_id */
	bs_ue(&b, 0);		/* vps_num_layer_sets_minus1 */
	bs_put(&b, 0, 1);	/* vps_timing_info_present_flag */
	bs_put(&b, 0, 1);	/* vps_extension_flag */

	bs_trailing(&b);
	return hevc_escape_nal(out, out_size, &b);
}

int
hevc_build_sps(uint8_t *out, size_t out_size,
	       const VAPictureParameterBufferHEVC *pic,
	       const VAIQMatrixBufferHEVC *iq)
{
	struct bs b;
	uint8_t rbsp[4096];
	unsigned int cfi;

	if (!out || !pic)
		return -1;
	cfi = pic->pic_fields.bits.chroma_format_idc;

	memset(rbsp, 0, sizeof(rbsp));
	b.buf = rbsp;
	b.size = sizeof(rbsp);
	b.bits = 0;
	b.overflow = 0;

	bs_put(&b, 0x42, 8);	/* NAL header byte 1: type=33 (SPS) */
	bs_put(&b, 0x01, 8);	/* NAL header byte 2: layer_id=0, tid_plus1=1 */
	bs_put(&b, 0, 4);	/* sps_video_parameter_set_id */
	bs_put(&b, 0, 3);	/* sps_max_sub_layers_minus1 */
	bs_put(&b, 1, 1);	/* sps_temporal_id_nesting_flag */
	hevc_profile_tier_level(&b, 1, 0,
			       pic->bit_depth_luma_minus8 ? 2 : 1, 1,
			       1,
			       hevc_level_for_size(pic));
	bs_ue(&b, 0);		/* sps_seq_parameter_set_id */
	bs_ue(&b, cfi);
	if (cfi == 3)
		bs_put(&b, pic->pic_fields.bits.separate_colour_plane_flag, 1);
	bs_ue(&b, pic->pic_width_in_luma_samples);
	bs_ue(&b, pic->pic_height_in_luma_samples);
	bs_put(&b, 0, 1);	/* conformance_window_flag */
	bs_ue(&b, pic->bit_depth_luma_minus8);
	bs_ue(&b, pic->bit_depth_chroma_minus8);
	bs_ue(&b, pic->log2_max_pic_order_cnt_lsb_minus4);
	/* VA-API omits the original reorder/latency syntax.  Bound reordering by
	 * the known DPB size and do not invent a latency limit. */
	bs_put(&b, 1, 1);
	bs_ue(&b, pic->sps_max_dec_pic_buffering_minus1);
	bs_ue(&b, hevc_reorder_pics(pic));
	bs_ue(&b, 0);
	bs_ue(&b, pic->log2_min_luma_coding_block_size_minus3);
	bs_ue(&b, pic->log2_diff_max_min_luma_coding_block_size);
	bs_ue(&b, pic->log2_min_transform_block_size_minus2);
	bs_ue(&b, pic->log2_diff_max_min_transform_block_size);
	bs_ue(&b, pic->max_transform_hierarchy_depth_inter);
	bs_ue(&b, pic->max_transform_hierarchy_depth_intra);
	bs_put(&b, pic->pic_fields.bits.scaling_list_enabled_flag, 1);
	if (pic->pic_fields.bits.scaling_list_enabled_flag) {
		bs_put(&b, iq != NULL, 1); /* sps_scaling_list_data_present_flag */
		if (iq)
			hevc_scaling_list_data(&b, iq);
	}
	bs_put(&b, pic->pic_fields.bits.amp_enabled_flag, 1);
	bs_put(&b, pic->slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag,
	       1);
	bs_put(&b, pic->pic_fields.bits.pcm_enabled_flag, 1);
	if (pic->pic_fields.bits.pcm_enabled_flag) {
		bs_put(&b, pic->pcm_sample_bit_depth_luma_minus1, 4);
		bs_put(&b, pic->pcm_sample_bit_depth_chroma_minus1, 4);
		bs_ue(&b, pic->log2_min_pcm_luma_coding_block_size_minus3);
		bs_ue(&b, pic->log2_diff_max_min_pcm_luma_coding_block_size);
		bs_put(&b, pic->pic_fields.bits.pcm_loop_filter_disabled_flag, 1);
	}
	/* VA short-slice mode carries the short-term RPS in each slice header;
	 * the picture buffer only reports how many sets the original long-form
	 * SPS had, not their syntax.  Advertising those unavailable sets here
	 * would emit an SPS with missing st_ref_pic_set() payloads. */
	bs_ue(&b, 0);
	bs_put(&b, pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag,
	       1);
	/* As with short-term RPS data, the VA picture buffer exposes the number
	 * of SPS long-term references but not their POC/used flags.  Keep the
	 * syntax valid and let slices carry their own long-term references. */
	if (pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag)
		bs_ue(&b, 0);
	bs_put(&b, pic->slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag, 1);
	bs_put(&b, pic->pic_fields.bits.strong_intra_smoothing_enabled_flag, 1);
	/* VUI/timing/colour metadata is absent from the VA picture buffer. */
	bs_put(&b, 0, 1);	/* vui_parameters_present_flag */
	bs_put(&b, 0, 1);	/* sps_extension_present_flag */

	bs_trailing(&b);
	return hevc_escape_nal(out, out_size, &b);
}

int
hevc_build_pps_id(uint8_t *out, size_t out_size,
		  const VAPictureParameterBufferHEVC *pic,
		  unsigned int pps_id)
{
	struct bs b;
	uint8_t rbsp[512];

	if (!out || !pic || pps_id > 63 || pic->num_extra_slice_header_bits > 7 ||
	    pic->num_tile_columns_minus1 > 19 ||
	    pic->num_tile_rows_minus1 > 21)
		return -1;

	memset(rbsp, 0, sizeof(rbsp));
	b.buf = rbsp;
	b.size = sizeof(rbsp);
	b.bits = 0;
	b.overflow = 0;

	bs_put(&b, 0x44, 8);	/* NAL header byte 1: type=34 (PPS) */
	bs_put(&b, 0x01, 8);	/* NAL header byte 2: layer_id=0, tid_plus1=1 */
	bs_ue(&b, pps_id);	/* pps_pic_parameter_set_id */
	bs_ue(&b, 0);		/* pps_seq_parameter_set_id */
	bs_put(&b, pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag,
	       1);
	bs_put(&b, pic->slice_parsing_fields.bits.output_flag_present_flag, 1);
	bs_put(&b, pic->num_extra_slice_header_bits, 3);
	bs_put(&b, pic->pic_fields.bits.sign_data_hiding_enabled_flag, 1);
	bs_put(&b, pic->slice_parsing_fields.bits.cabac_init_present_flag, 1);
	bs_ue(&b, pic->num_ref_idx_l0_default_active_minus1);
	bs_ue(&b, pic->num_ref_idx_l1_default_active_minus1);
	bs_se(&b, pic->init_qp_minus26);
	bs_put(&b, pic->pic_fields.bits.constrained_intra_pred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.transform_skip_enabled_flag, 1);
	bs_put(&b, pic->pic_fields.bits.cu_qp_delta_enabled_flag, 1);
	if (pic->pic_fields.bits.cu_qp_delta_enabled_flag)
		bs_ue(&b, pic->diff_cu_qp_delta_depth);
	bs_se(&b, pic->pps_cb_qp_offset);
	bs_se(&b, pic->pps_cr_qp_offset);
	bs_put(&b, pic->slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag,
	       1);
	bs_put(&b, pic->pic_fields.bits.weighted_pred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.weighted_bipred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.transquant_bypass_enabled_flag, 1);
	bs_put(&b, pic->pic_fields.bits.tiles_enabled_flag, 1);
	bs_put(&b, pic->pic_fields.bits.entropy_coding_sync_enabled_flag, 1);
	if (pic->pic_fields.bits.tiles_enabled_flag) {
		bs_ue(&b, pic->num_tile_columns_minus1);
		bs_ue(&b, pic->num_tile_rows_minus1);
		/* VA exposes the resolved column and row widths, but not
		 * uniform_spacing_flag.  Explicit widths reproduce either form. */
		bs_put(&b, 0, 1);	/* uniform_spacing_flag */
		for (unsigned int i = 0; i < pic->num_tile_columns_minus1; i++)
			bs_ue(&b, pic->column_width_minus1[i]);
		for (unsigned int i = 0; i < pic->num_tile_rows_minus1; i++)
			bs_ue(&b, pic->row_height_minus1[i]);
		bs_put(&b, pic->pic_fields.bits.loop_filter_across_tiles_enabled_flag,
		       1);
	}
	bs_put(&b, pic->pic_fields.bits.pps_loop_filter_across_slices_enabled_flag, 1);
	bs_put(&b, pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
		       pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag ||
		       pic->pps_beta_offset_div2 || pic->pps_tc_offset_div2,
	       1);
	if (pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag ||
	    pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag ||
	    pic->pps_beta_offset_div2 || pic->pps_tc_offset_div2) {
		bs_put(&b, pic->slice_parsing_fields.bits.deblocking_filter_override_enabled_flag, 1);
		bs_put(&b, pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag, 1);
		if (!pic->slice_parsing_fields.bits.pps_disable_deblocking_filter_flag) {
			bs_se(&b, pic->pps_beta_offset_div2);
			bs_se(&b, pic->pps_tc_offset_div2);
		}
	}
	bs_put(&b, 0, 1);	/* pps_scaling_list_data_present_flag */
	bs_put(&b, pic->slice_parsing_fields.bits.lists_modification_present_flag, 1);
	bs_ue(&b, pic->log2_parallel_merge_level_minus2);
	bs_put(&b, pic->slice_parsing_fields.bits.slice_segment_header_extension_present_flag,
	       1);
	bs_put(&b, 0, 1);	/* pps_extension_present_flag */

	bs_trailing(&b);
	return hevc_escape_nal(out, out_size, &b);
}

int
hevc_build_pps(uint8_t *out, size_t out_size,
	       const VAPictureParameterBufferHEVC *pic)
{
	return hevc_build_pps_id(out, out_size, pic, 0);
}
