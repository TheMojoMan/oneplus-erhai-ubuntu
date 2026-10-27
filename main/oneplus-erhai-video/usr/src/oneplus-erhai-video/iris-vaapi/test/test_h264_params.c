// SPDX-License-Identifier: GPL-2.0-or-later
/* Parse a real H.264 SPS/PPS/slice from an Annex-B file into
 * VAPictureParameterBufferH264, re-serialize SPS/PPS, and write a stream
 * that ffmpeg must decode correctly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <va/va.h>
#include "h264_params.h"

struct br {
	const uint8_t *p;
	const uint8_t *end;
	unsigned int bit;
};

static unsigned int br_bit(struct br *b)
{
	unsigned int v = (b->p[0] >> (7 - b->bit)) & 1;

	if (++b->bit == 8) {
		b->bit = 0;
		b->p++;
	}
	return v;
}

static unsigned int br_bits(struct br *b, int n)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < n; i++)
		v = (v << 1) | br_bit(b);
	return v;
}

static unsigned int br_ue(struct br *b)
{
	unsigned int z = 0, v;

	while (!br_bit(b))
		z++;
	v = (1u << z) - 1;
	while (z--)
		v += br_bit(b) << z;
	return v;
}

static int br_se(struct br *b)
{
	unsigned int u = br_ue(b);

	return (u & 1) ? (int)((u + 1) >> 1) : -(int)(u >> 1);
}

static int serializer_selftest(void)
{
	VAPictureParameterBufferH264 pic;
	VAIQMatrixBufferH264 iq;
	uint8_t out[4096];
	/* first_mb=0, P slice, pps=0, frame_num=0, then override flag */
	static const uint8_t p_default[] = { 0x41, 0xe0 };
	static const uint8_t p_override[] = { 0x41, 0xe1 };
	int n;

	memset(&pic, 0, sizeof(pic));
	memset(&iq, 16, sizeof(iq));
	pic.seq_fields.bits.chroma_format_idc = 1;
	pic.seq_fields.bits.frame_mbs_only_flag = 1;
	pic.seq_fields.bits.direct_8x8_inference_flag = 1;
	pic.pic_fields.bits.transform_8x8_mode_flag = 1;
	pic.picture_width_in_mbs_minus1 = 19;
	pic.picture_height_in_mbs_minus1 = 14;
	pic.num_ref_frames = 4;
	n = h264_build_sps(out, sizeof(out), &pic, VAProfileH264High, 320, 240);
	if (n <= 0 ||
	    h264_build_sps(out, 2, &pic, VAProfileH264High, 320, 240) >= 0)
		return 1;
	if (h264_build_sps(out, sizeof(out), NULL, VAProfileH264High,
			   320, 240) >= 0)
		return 1;
	n = h264_build_sps(out, sizeof(out), &pic, VAProfileH264Main, 320, 240);
	if (n <= 0)
		return 1;
	n = h264_build_pps(out, sizeof(out), &pic, &iq, 1, 0);
	if (n <= 0 || h264_build_pps(out, 2, &pic, &iq, 1, 0) >= 0)
		return 1;
	pic.seq_fields.bits.pic_order_cnt_type = 2;
	if (h264_slice_default_ref_mask(p_default, sizeof(p_default), &pic) != 1 ||
	    h264_slice_default_ref_mask(p_override, sizeof(p_override), &pic) != 0 ||
	    h264_slice_default_ref_mask(p_default, 1, &pic) >= 0)
		return 1;
	pic.seq_fields.bits.pic_order_cnt_type = 1;
	if (h264_build_sps(out, sizeof(out), &pic, VAProfileH264High,
			   320, 240) >= 0)
		return 1;
	return 0;
}

static long find_start(const uint8_t *buf, size_t size, size_t from)
{
	size_t i;

	for (i = from; i + 3 < size; i++) {
		if (i + 4 < size && buf[i] == 0 && buf[i + 1] == 0 &&
		    buf[i + 2] == 0 && buf[i + 3] == 1)
			return i + 4;
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 &&
		    (i == from || buf[i - 1] != 0))
			return i + 3;
	}
	return -1;
}

static void parse_sps(const uint8_t *nal, size_t len, VAPictureParameterBufferH264 *pic)
{
	struct br b;
	unsigned int u;

	memset(pic, 0, sizeof(*pic));
	b.p = nal + 1;
	b.end = nal + len;
	b.bit = 0;

	(void)br_bits(&b, 8);	/* profile_idc */
	(void)br_bits(&b, 8);	/* constraint flags */
	(void)br_bits(&b, 8);	/* level_idc */
	(void)br_ue(&b);	/* sps_id */

	u = br_ue(&b);		/* chroma_format_idc */
	pic->seq_fields.bits.chroma_format_idc = u;
	if (u == 3)
		pic->seq_fields.bits.residual_colour_transform_flag = br_bit(&b);
	pic->bit_depth_luma_minus8 = br_ue(&b);
	pic->bit_depth_chroma_minus8 = br_ue(&b);
	(void)br_bit(&b);	/* qpprime_y_zero_transform_bypass_flag */
	if (br_bit(&b))		/* seq_scaling_matrix_present_flag */
		return;		/* not supported in test streams */

	pic->seq_fields.bits.log2_max_frame_num_minus4 = br_ue(&b);
	u = br_ue(&b);
	pic->seq_fields.bits.pic_order_cnt_type = u;
	if (u == 0) {
		pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = br_ue(&b);
	} else if (u == 1) {
		pic->seq_fields.bits.delta_pic_order_always_zero_flag = br_bit(&b);
		(void)br_se(&b);
		(void)br_se(&b);
		(void)br_ue(&b);
	}

	pic->num_ref_frames = br_ue(&b);
	pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag = br_bit(&b);
	pic->picture_width_in_mbs_minus1 = br_ue(&b);
	pic->picture_height_in_mbs_minus1 = br_ue(&b);
	pic->seq_fields.bits.frame_mbs_only_flag = br_bit(&b);
	if (!pic->seq_fields.bits.frame_mbs_only_flag)
		pic->seq_fields.bits.mb_adaptive_frame_field_flag = br_bit(&b);
	pic->seq_fields.bits.direct_8x8_inference_flag = br_bit(&b);
	if (br_bit(&b)) {	/* frame_cropping_flag */
		(void)br_ue(&b); (void)br_ue(&b); (void)br_ue(&b); (void)br_ue(&b);
	}
}

static void parse_pps(const uint8_t *nal, size_t len, VAPictureParameterBufferH264 *pic)
{
	struct br b;
	unsigned int cfi = pic->seq_fields.bits.chroma_format_idc;

	b.p = nal + 1;
	b.end = nal + len;
	b.bit = 0;

	(void)br_ue(&b);	/* pps_id */
	(void)br_ue(&b);	/* sps_id */
	pic->pic_fields.bits.entropy_coding_mode_flag = br_bit(&b);
	pic->pic_fields.bits.pic_order_present_flag = br_bit(&b);
	(void)br_ue(&b);	/* num_slice_groups_minus1 */
	(void)br_ue(&b);	/* num_ref_idx_l0 */
	(void)br_ue(&b);	/* num_ref_idx_l1 */
	pic->pic_fields.bits.weighted_pred_flag = br_bit(&b);
	pic->pic_fields.bits.weighted_bipred_idc = br_bits(&b, 2);
	pic->pic_init_qp_minus26 = br_se(&b);
	pic->pic_init_qs_minus26 = br_se(&b);
	pic->chroma_qp_index_offset = br_se(&b);
	pic->pic_fields.bits.deblocking_filter_control_present_flag = br_bit(&b);
	pic->pic_fields.bits.constrained_intra_pred_flag = br_bit(&b);
	pic->pic_fields.bits.redundant_pic_cnt_present_flag = br_bit(&b);
	if (cfi == 1)
		pic->pic_fields.bits.transform_8x8_mode_flag = br_bit(&b);
	(void)br_bit(&b);	/* pic_scaling_matrix_present_flag */
	pic->second_chroma_qp_index_offset = br_se(&b);
}

int main(int argc, char **argv)
{
	const char *path;
	const char *outpath = argc > 2 ? argv[2] : "/tmp/reconstructed.h264";
	FILE *fp, *out;
	uint8_t *data;
	long fsize;
	long pos;
	uint8_t sps_nal[256], pps_nal[64], slice_nal[1 << 20];
	int sps_len = -1, pps_len = -1, slice_len = -1;
	VAPictureParameterBufferH264 pic;
	uint8_t out_sps[256], out_pps[64];
	int out_sps_len, out_pps_len;

	if (argc < 2) {
		return serializer_selftest();
	}
	path = argv[1];

	fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	data = malloc(fsize);
	if (!data || fread(data, 1, fsize, fp) != (size_t)fsize)
		return 1;
	fclose(fp);

	/* Extract first SPS, PPS and VCL slice. */
	pos = find_start(data, fsize, 0);
	while (pos >= 0) {
		long next = find_start(data, fsize, pos + 1);
		int t = data[pos] & 0x1f;
		size_t end = next < 0 ? (size_t)fsize : (size_t)next;
		size_t nalsz = end - pos;

				if (t == 7 && sps_len < 0) {
			if (nalsz > sizeof(sps_nal)) { fprintf(stderr, "SPS too big %zu\n", nalsz); return 1; }
			memcpy(sps_nal, data + pos, nalsz);
			sps_len = nalsz;
		} else if (t == 8 && pps_len < 0) {
			if (nalsz > sizeof(pps_nal)) { fprintf(stderr, "PPS too big %zu\n", nalsz); return 1; }
			memcpy(pps_nal, data + pos, nalsz);
			pps_len = nalsz;
		} else if (t >= 1 && t <= 5 && slice_len < 0) {
			if (nalsz > sizeof(slice_nal)) { fprintf(stderr, "slice too big %zu\n", nalsz); return 1; }
			memcpy(slice_nal, data + pos, nalsz);
			slice_len = nalsz;
		}
		if (sps_len >= 0 && pps_len >= 0 && slice_len >= 0)
			break;
		pos = next;
	}
	free(data);

	printf("orig SPS %d bytes, PPS %d bytes, slice %d bytes\n",
	       sps_len, pps_len, slice_len);

	parse_sps(sps_nal, sps_len, &pic);
	parse_pps(pps_nal, pps_len, &pic);

	printf("parsed: %ux%u mbs, chroma=%u, poc_type=%u, refs=%u, frame_mbs_only=%u\n",
	       (pic.picture_width_in_mbs_minus1 + 1) * 16,
	       (pic.picture_height_in_mbs_minus1 + 1) * 16,
	       pic.seq_fields.bits.chroma_format_idc,
	       pic.seq_fields.bits.pic_order_cnt_type,
	       pic.num_ref_frames,
	       pic.seq_fields.bits.frame_mbs_only_flag);
	printf("first slice default-ref mask=%d\n",
	       h264_slice_default_ref_mask(slice_nal, slice_len, &pic));

	out_sps_len = h264_build_sps(out_sps, sizeof(out_sps), &pic,
				    VAProfileH264High,
				    (pic.picture_width_in_mbs_minus1 + 1) * 16,
				    (pic.picture_height_in_mbs_minus1 + 1) * 16);
	out_pps_len = h264_build_pps(out_pps, sizeof(out_pps), &pic, NULL, 1, 0);
	printf("rebuilt SPS %d bytes, PPS %d bytes\n", out_sps_len, out_pps_len);

	out = fopen(outpath, "wb");
	if (!out) { perror(outpath); return 1; }
	fwrite("\x00\x00\x00\x01", 1, 4, out);
	fwrite(out_sps, 1, out_sps_len, out);
	fwrite("\x00\x00\x00\x01", 1, 4, out);
	fwrite(out_pps, 1, out_pps_len, out);
	fwrite("\x00\x00\x00\x01", 1, 4, out);
	fwrite(slice_nal, 1, slice_len, out);
	fclose(out);
	printf("wrote %s\n", outpath);
	return 0;
}
