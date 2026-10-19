// SPDX-License-Identifier: GPL-2.0-or-later

#include <string.h>

#include "h264_params.h"

/* ---- RBSP bit writer ---- */

struct bs {
	uint8_t *buf;
	size_t size;
	int bits;		/* total bits written */
	int overflow;
};

struct br {
	const uint8_t *buf;
	size_t size;
	size_t byte;
	unsigned int bit;
	unsigned int zero_bytes;
	uint8_t current;
};

static int br_next_byte(struct br *b)
{
	uint8_t value;

	while (b->byte < b->size) {
		value = b->buf[b->byte++];
		if (b->zero_bytes >= 2 && value == 3) {
			b->zero_bytes = 0;
			continue;
		}
		b->zero_bytes = value == 0 ? b->zero_bytes + 1 : 0;
		b->current = value;
		b->bit = 0;
		return 0;
	}
	return -1;
}

static int br_bit(struct br *b, unsigned int *value)
{
	if (b->bit >= 8 && br_next_byte(b))
		return -1;
	*value = (b->current >> (7 - b->bit++)) & 1;
	return 0;
}

static int br_bits(struct br *b, unsigned int count, unsigned int *value)
{
	unsigned int bit;

	*value = 0;
	while (count--) {
		if (br_bit(b, &bit))
			return -1;
		*value = (*value << 1) | bit;
	}
	return 0;
}

static int br_ue(struct br *b, unsigned int *value)
{
	unsigned int bit, suffix = 0, zeros = 0;

	while (1) {
		if (br_bit(b, &bit))
			return -1;
		if (bit)
			break;
		if (++zeros >= 32)
			return -1;
	}
	if (zeros && br_bits(b, zeros, &suffix))
		return -1;
	*value = ((1u << zeros) - 1) + suffix;
	return 0;
}

static int br_se(struct br *b)
{
	unsigned int value;

	return br_ue(b, &value);
}

int h264_slice_default_ref_mask(const uint8_t *nal, size_t nal_size,
				const VAPictureParameterBufferH264 *pic)
{
	struct br b;
	unsigned int value, slice_type, field_pic = 0;
	unsigned int nal_type;

	if (!nal || nal_size < 2 || !pic)
		return -1;
	nal_type = nal[0] & 0x1f;
	if (nal_type < 1 || nal_type > 5)
		return -1;
	b.buf = nal + 1;
	b.size = nal_size - 1;
	b.byte = 0;
	b.bit = 8;
	b.zero_bytes = 0;
	b.current = 0;

	if (br_ue(&b, &value) || br_ue(&b, &slice_type) ||
	    br_ue(&b, &value))
		return -1;
	slice_type %= 5;
	if (pic->seq_fields.bits.chroma_format_idc == 3 &&
	    br_bits(&b, 2, &value))
		return -1;
	if (br_bits(&b, pic->seq_fields.bits.log2_max_frame_num_minus4 + 4,
		    &value))
		return -1;
	if (!pic->seq_fields.bits.frame_mbs_only_flag) {
		if (br_bit(&b, &field_pic))
			return -1;
		if (field_pic && br_bit(&b, &value))
			return -1;
	}
	if (nal_type == 5 && br_ue(&b, &value))
		return -1;
	if (pic->seq_fields.bits.pic_order_cnt_type == 0) {
		if (br_bits(&b,
			    pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 + 4,
			    &value))
			return -1;
		if (pic->pic_fields.bits.pic_order_present_flag && !field_pic &&
		    br_se(&b))
			return -1;
	} else if (pic->seq_fields.bits.pic_order_cnt_type == 1 &&
		   !pic->seq_fields.bits.delta_pic_order_always_zero_flag) {
		if (br_se(&b))
			return -1;
		if (pic->pic_fields.bits.pic_order_present_flag && !field_pic &&
		    br_se(&b))
			return -1;
	}
	if (pic->pic_fields.bits.redundant_pic_cnt_present_flag &&
	    br_ue(&b, &value))
		return -1;
	if (slice_type == 1 && br_bit(&b, &value))
		return -1;
	if (slice_type != 0 && slice_type != 1 && slice_type != 3)
		return 0;
	if (br_bit(&b, &value))
		return -1;
	if (value)
		return 0;
	return slice_type == 1 ? 3 : 1;
}

static void bs_put(struct bs *b, unsigned int val, int n)
{
	int i;

	for (i = n - 1; i >= 0; i--) {
		int byte = b->bits >> 3;
		int bit = 7 - (b->bits & 7);

		if (byte >= (int)b->size) {
			b->overflow = 1;
			return;
		}
		if (val & (1u << i))
			b->buf[byte] |= (1u << bit);
		b->bits++;
	}
}

static void bs_ue(struct bs *b, unsigned int val)
{
	unsigned int code;
	int n;

	if (val == UINT32_MAX) {
		b->overflow = 1;
		return;
	}
	code = val + 1;
	n = 32 - __builtin_clz(code);

	bs_put(b, 0, n - 1);
	bs_put(b, code, n);
}

static void bs_se(struct bs *b, int val);

static const uint8_t zigzag4x4[16] = {
	0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15,
};

static const uint8_t zigzag8x8[64] = {
	0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
	12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
	35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
	58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63,
};

static void
bs_scaling_list(struct bs *b, const uint8_t *matrix,
		const uint8_t *scan, unsigned int count)
{
	int last = 8;
	unsigned int i;

	for (i = 0; i < count; i++) {
		int next = matrix[scan[i]];
		int delta = next - last;

		if (delta > 127)
			delta -= 256;
		else if (delta < -128)
			delta += 256;
		bs_se(b, delta);
		last = next;
	}
}

static void bs_se(struct bs *b, int val)
{
	unsigned int code = (val <= 0) ? (unsigned int)(-2 * val)
				      : (unsigned int)(2 * val - 1);

	bs_ue(b, code);
}

/* ---- profile mapping ---- */

static int profile_idc(VAProfile profile)
{
	switch (profile) {
	case VAProfileH264ConstrainedBaseline:
		return 66;
	case VAProfileH264Main:
		return 77;
	default:
		/* High and any newer H.264 profile use the High syntax here.
		 * Keep this independent of enum values added by newer libva. */
		return 100;
	}
}

static int constraint_flags(VAProfile profile)
{
	return profile == VAProfileH264ConstrainedBaseline ? 0x40 : 0;
}

/* Pick a level from the coded macroblock dimensions.  Height-only selection
 * incorrectly labels wide 4K/8K streams and makes the generated SPS reject
 * otherwise valid pictures. */
static int level_for_size(unsigned int width, unsigned int height)
{
	uint64_t mb = ((uint64_t)width + 15) / 16 *
		      (((uint64_t)height + 15) / 16);
	uint64_t mbps = mb * 30;
	static const struct {
		unsigned int level, max_mbps, max_fs;
	} limits[] = {
		{ 10, 1485, 99 }, { 11, 3000, 396 }, { 12, 6000, 396 },
		{ 13, 11880, 396 }, { 21, 19800, 792 }, { 22, 20250, 1620 },
		{ 30, 40500, 1620 }, { 31, 108000, 3600 },
		{ 32, 216000, 5120 }, { 40, 245760, 8192 },
		{ 42, 522240, 8704 }, { 50, 589824, 22080 },
		{ 51, 983040, 36864 },
	};
	unsigned int i;

	for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++)
		if (mb <= limits[i].max_fs && mbps <= limits[i].max_mbps)
			return limits[i].level;
	return 52;
}

int h264_build_sps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferH264 *pic, VAProfile profile,
		   unsigned int display_width, unsigned int display_height)
{
	struct bs b;
	unsigned int sps_id = 0;
	unsigned int cfi;
	unsigned int coded_width, coded_height;
	unsigned int frame_factor;
	int high_profile;

	if (!out || !pic || pic->seq_fields.bits.pic_order_cnt_type == 1)
		return -1;
	cfi = pic->seq_fields.bits.chroma_format_idc;
	high_profile = profile_idc(profile) >= 100;
	frame_factor = 2U - pic->seq_fields.bits.frame_mbs_only_flag;
	coded_width = (pic->picture_width_in_mbs_minus1 + 1) * 16;
	coded_height = (pic->picture_height_in_mbs_minus1 + 1) * 16 *
		frame_factor;
	if (!display_width)
		display_width = coded_width;
	if (!display_height)
		display_height = coded_height;
	if (!high_profile && (cfi != 1 || pic->bit_depth_luma_minus8 ||
			      pic->bit_depth_chroma_minus8))
		return -1;

	memset(out, 0, out_size);
	b.buf = out;
	b.size = out_size;
	b.bits = 0;
	b.overflow = 0;

	bs_put(&b, 0x67, 8);	/* NAL header: forbidden=0, ref_idc=3, type=7 */
	bs_put(&b, profile_idc(profile), 8);
	bs_put(&b, constraint_flags(profile), 8);
	bs_put(&b, level_for_size(coded_width, coded_height), 8);
	bs_ue(&b, sps_id);
	if (high_profile) {
		bs_ue(&b, cfi);
		if (cfi == 3)
			bs_put(&b,
			       pic->seq_fields.bits.residual_colour_transform_flag, 1);
		bs_ue(&b, pic->bit_depth_luma_minus8);
		bs_ue(&b, pic->bit_depth_chroma_minus8);
		bs_put(&b, 0, 1); /* qpprime_y_zero_transform_bypass_flag */
		bs_put(&b, 0, 1); /* seq_scaling_matrix_present_flag */
	}

	bs_ue(&b, pic->seq_fields.bits.log2_max_frame_num_minus4);
	bs_ue(&b, pic->seq_fields.bits.pic_order_cnt_type);
	if (pic->seq_fields.bits.pic_order_cnt_type == 0) {
		bs_ue(&b, pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4);
	}
	bs_ue(&b, pic->num_ref_frames);
	bs_put(&b, pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag,
	       1);
	bs_ue(&b, pic->picture_width_in_mbs_minus1);
	bs_ue(&b, pic->picture_height_in_mbs_minus1);
	bs_put(&b, pic->seq_fields.bits.frame_mbs_only_flag, 1);
	if (!pic->seq_fields.bits.frame_mbs_only_flag)
		bs_put(&b, pic->seq_fields.bits.mb_adaptive_frame_field_flag, 1);
	bs_put(&b, pic->seq_fields.bits.direct_8x8_inference_flag, 1);
	if (display_width <= coded_width && display_height <= coded_height) {
		unsigned int sub_width = cfi == 1 || cfi == 2 ? 2 : 1;
		unsigned int sub_height = cfi == 1 ? 2 : 1;
		unsigned int crop_x = cfi ? sub_width : 1;
		unsigned int crop_y = cfi ? sub_height * frame_factor :
			frame_factor;
		unsigned int right = coded_width - display_width;
		unsigned int bottom = coded_height - display_height;

		if ((right || bottom) && !(right % crop_x) && !(bottom % crop_y)) {
			bs_put(&b, 1, 1);	/* frame_cropping_flag */
			bs_ue(&b, 0);		/* frame_crop_left_offset */
			bs_ue(&b, right / crop_x);
			bs_ue(&b, 0);		/* frame_crop_top_offset */
			bs_ue(&b, bottom / crop_y);
		} else {
			bs_put(&b, 0, 1);	/* frame_cropping_flag */
		}
	} else {
		bs_put(&b, 0, 1);	/* frame_cropping_flag */
	}

	/* Timing, colour and reorder restrictions are not carried by the VA
	 * picture buffer.  Decode-order output is selected through V4L2 controls,
	 * so do not fabricate a VUI here. */
	bs_put(&b, 0, 1);	/* vui_parameters_present_flag */

	/* rbsp_stop_one_bit + alignment */
	bs_put(&b, 1, 1);
	while (b.bits & 7)
		bs_put(&b, 0, 1);

	return b.overflow ? -1 : b.bits >> 3;
}

int h264_build_pps(uint8_t *out, size_t out_size,
		   const VAPictureParameterBufferH264 *pic,
		   const VAIQMatrixBufferH264 *iq,
		   int num_ref_idx_l0_default_active_minus1,
		   int num_ref_idx_l1_default_active_minus1)
{
	struct bs b;
	unsigned int sps_id = 0;
	unsigned int pps_id = 0;
	unsigned int cfi;
	unsigned int i;
	int have_scaling = 0;

	if (!out || !pic)
		return -1;
	cfi = pic->seq_fields.bits.chroma_format_idc;
	if (iq) {
		const uint8_t *values = (const uint8_t *)iq;
		size_t matrix_size = sizeof(iq->ScalingList4x4) +
			sizeof(iq->ScalingList8x8);

		for (i = 0; i < matrix_size; i++)
			if (values[i] != 16) {
				have_scaling = 1;
				break;
			}
	}

	memset(out, 0, out_size);
	b.buf = out;
	b.size = out_size;
	b.bits = 0;
	b.overflow = 0;

	bs_put(&b, 0x68, 8);	/* NAL header: forbidden=0, ref_idc=3, type=8 */
	bs_ue(&b, pps_id);
	bs_ue(&b, sps_id);
	bs_put(&b, pic->pic_fields.bits.entropy_coding_mode_flag, 1);
	bs_put(&b, pic->pic_fields.bits.pic_order_present_flag, 1);
	bs_ue(&b, 0);		/* num_slice_groups_minus1 */
	bs_ue(&b, num_ref_idx_l0_default_active_minus1);
	bs_ue(&b, num_ref_idx_l1_default_active_minus1);
	bs_put(&b, pic->pic_fields.bits.weighted_pred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.weighted_bipred_idc, 2);
	bs_se(&b, pic->pic_init_qp_minus26);
	bs_se(&b, pic->pic_init_qs_minus26);
	bs_se(&b, pic->chroma_qp_index_offset);
	bs_put(&b, pic->pic_fields.bits.deblocking_filter_control_present_flag,
	       1);
	bs_put(&b, pic->pic_fields.bits.constrained_intra_pred_flag, 1);
	bs_put(&b, pic->pic_fields.bits.redundant_pic_cnt_present_flag, 1);
	if (cfi == 1)
		bs_put(&b, pic->pic_fields.bits.transform_8x8_mode_flag, 1);
	bs_put(&b, have_scaling, 1);	/* pic_scaling_matrix_present_flag */
	if (have_scaling) {
		for (i = 0; i < 6; i++) {
			bs_put(&b, 1, 1);
			bs_scaling_list(&b, iq->ScalingList4x4[i], zigzag4x4,
					16);
		}
		if (pic->pic_fields.bits.transform_8x8_mode_flag)
			for (i = 0; i < 2; i++) {
				bs_put(&b, 1, 1);
				bs_scaling_list(&b, iq->ScalingList8x8[i],
						zigzag8x8, 64);
			}
	}
	bs_se(&b, pic->second_chroma_qp_index_offset);

	bs_put(&b, 1, 1);
	while (b.bits & 7)
		bs_put(&b, 0, 1);

	return b.overflow ? -1 : b.bits >> 3;
}
