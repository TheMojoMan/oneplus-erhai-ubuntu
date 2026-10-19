// SPDX-License-Identifier: GPL-2.0-or-later
/* Validate HEVC VPS/SPS/PPS fallback serialization from a real
 * VAPictureParameterBufferHEVC.  Fields unavailable through VA-API (notably
 * VUI timing) must remain absent instead of being fabricated.
 */

#include <stdio.h>
#include <string.h>
#include <va/va.h>
#include "hevc_params.h"

static int check_nal(const char *name, const uint8_t *got, int got_len,
		     const uint8_t *want, size_t want_len)
{
	if (got_len == (int)want_len && !memcmp(got, want, want_len))
		return 0;

	fprintf(stderr, "%s mismatch\n  got (%d): ", name, got_len);
	for (int i = 0; i < got_len; i++)
		fprintf(stderr, "%02x", got[i]);
	fprintf(stderr, "\n  want(%zu): ", want_len);
	for (size_t i = 0; i < want_len; i++)
		fprintf(stderr, "%02x", want[i]);
	fputc('\n', stderr);
	return 1;
}

int main(void)
{
	/* Canonical fallback NALs for the matching x265 1080p picture fields. */
	static const uint8_t expected_vps[] = {
		0x40, 0x01, 0x0c, 0x01, 0xff, 0xff, 0x01, 0x60,
		0x00, 0x00, 0x03, 0x00, 0x90, 0x00, 0x00, 0x03,
		0x00, 0x00, 0x03, 0x00, 0x78, 0x95, 0xc0, 0x90,
	};
	static const uint8_t expected_sps[] = {
		0x42, 0x01, 0x01, 0x01, 0x60, 0x00, 0x00, 0x03,
		0x00, 0x90, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03,
		0x00, 0x78, 0xa0, 0x03, 0xc0, 0x80, 0x10, 0xe5,
		0x96, 0x57, 0x92, 0x4c, 0xac, 0x80,
	};
	static const uint8_t expected_pps[] = {
		0x44, 0x01, 0xc1, 0x72, 0xb4, 0x62, 0x40,
	};
	static const uint8_t expected_conditional_pps[] = {
		0x44, 0x01, 0xc7, 0x72, 0xbc, 0xda, 0x0a, 0x0a,
		0x0f, 0xf1, 0x48, 0x48,
	};
	VAPictureParameterBufferHEVC pic;
	VAIQMatrixBufferHEVC iq;
	uint8_t out[512];
	int n, failed = 0;

	memset(&pic, 0, sizeof(pic));
	pic.pic_width_in_luma_samples = 1920;
	pic.pic_height_in_luma_samples = 1080;
	pic.pic_fields.bits.chroma_format_idc = 1;
	pic.bit_depth_luma_minus8 = 0;
	pic.bit_depth_chroma_minus8 = 0;
	memset(&iq, 16, sizeof(iq));
	pic.pic_fields.bits.scaling_list_enabled_flag = 1;
	n = hevc_build_sps(out, sizeof(out), &pic, &iq);
	if (n <= (int)sizeof(expected_sps) ||
	    hevc_build_sps(out, 32, &pic, &iq) >= 0) {
		fprintf(stderr, "HEVC scaling-list serialization failed\n");
		failed = 1;
	}
	pic.pic_fields.bits.scaling_list_enabled_flag = 0;
	pic.log2_max_pic_order_cnt_lsb_minus4 = 4;
	pic.sps_max_dec_pic_buffering_minus1 = 4;
	pic.log2_min_luma_coding_block_size_minus3 = 0;
	pic.log2_diff_max_min_luma_coding_block_size = 3;
	pic.log2_min_transform_block_size_minus2 = 0;
	pic.log2_diff_max_min_transform_block_size = 3;
	pic.max_transform_hierarchy_depth_intra = 0;
	pic.max_transform_hierarchy_depth_inter = 0;
	pic.slice_parsing_fields.bits.sample_adaptive_offset_enabled_flag = 1;
	pic.slice_parsing_fields.bits.sps_temporal_mvp_enabled_flag = 1;
	pic.pic_fields.bits.strong_intra_smoothing_enabled_flag = 1;
	pic.pic_fields.bits.pcm_enabled_flag = 0;
	pic.num_short_term_ref_pic_sets = 0;

	/* PPS fields matching the original stream. */
	pic.pic_fields.bits.sign_data_hiding_enabled_flag = 1;
	pic.pic_fields.bits.cu_qp_delta_enabled_flag = 1;
	pic.diff_cu_qp_delta_depth = 1;
	pic.pic_fields.bits.weighted_pred_flag = 1;
	pic.pic_fields.bits.entropy_coding_sync_enabled_flag = 1;
	pic.pic_fields.bits.pps_loop_filter_across_slices_enabled_flag = 1;
	pic.log2_parallel_merge_level_minus2 = 0;
	pic.pps_cb_qp_offset = 0;
	pic.pps_cr_qp_offset = 0;

	n = hevc_build_vps(out, sizeof(out), &pic);
	failed |= check_nal("VPS", out, n, expected_vps,
			    sizeof(expected_vps));
	n = hevc_build_sps(out, sizeof(out), &pic, NULL);
	failed |= check_nal("SPS", out, n, expected_sps,
			    sizeof(expected_sps));
	n = hevc_build_pps(out, sizeof(out), &pic);
	failed |= check_nal("PPS", out, n, expected_pps,
			    sizeof(expected_pps));

	/* Main10 must be signalled in both profile_tier_level structures.  The
	 * profile_idc byte is immediately followed by compatibility flag 2. */
	pic.bit_depth_luma_minus8 = 2;
	pic.bit_depth_chroma_minus8 = 2;
	n = hevc_build_vps(out, sizeof(out), &pic);
	if (n < 8 || out[6] != 0x02 || out[7] != 0x20) {
		fprintf(stderr, "VPS did not signal Main10 profile\n");
		failed = 1;
	}
	n = hevc_build_sps(out, sizeof(out), &pic, NULL);
	if (n < 5 || out[3] != 0x02 || out[4] != 0x20) {
		fprintf(stderr, "SPS did not signal Main10 profile\n");
		failed = 1;
	}
	pic.bit_depth_luma_minus8 = 0;
	pic.bit_depth_chroma_minus8 = 0;

	/* Exercise conditional PPS syntax: extra header bits, chroma offsets,
	 * explicit tiles, entropy sync and deblocking offsets. */
	pic.num_extra_slice_header_bits = 3;
	pic.slice_parsing_fields.bits.pps_slice_chroma_qp_offsets_present_flag = 1;
	pic.pic_fields.bits.tiles_enabled_flag = 1;
	pic.num_tile_columns_minus1 = 2;
	pic.num_tile_rows_minus1 = 1;
	pic.column_width_minus1[0] = 9;
	pic.column_width_minus1[1] = 19;
	pic.row_height_minus1[0] = 14;
	pic.pic_fields.bits.loop_filter_across_tiles_enabled_flag = 1;
	pic.slice_parsing_fields.bits.deblocking_filter_override_enabled_flag = 1;
	pic.pps_beta_offset_div2 = -2;
	pic.pps_tc_offset_div2 = 2;
	n = hevc_build_pps(out, sizeof(out), &pic);
	failed |= check_nal("conditional PPS", out, n, expected_conditional_pps,
			    sizeof(expected_conditional_pps));

	/* All builders must reject an output buffer that cannot hold the NAL. */
	if (hevc_build_vps(out, 2, &pic) >= 0 ||
	    hevc_build_sps(out, 2, &pic, NULL) >= 0 ||
	    hevc_build_pps(out, 2, &pic) >= 0 ||
	    hevc_build_vps(NULL, sizeof(out), &pic) >= 0 ||
	    hevc_build_sps(out, sizeof(out), NULL, NULL) >= 0) {
		fprintf(stderr, "invalid builder input was not rejected\n");
		failed = 1;
	}
	pic.num_tile_columns_minus1 = 20;
	if (hevc_build_pps(out, sizeof(out), &pic) >= 0) {
		fprintf(stderr, "invalid tile count was not rejected\n");
		failed = 1;
	}

	return failed;
}
