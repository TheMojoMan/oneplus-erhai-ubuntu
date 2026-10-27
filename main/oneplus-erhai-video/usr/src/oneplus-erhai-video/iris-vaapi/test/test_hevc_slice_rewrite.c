// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "hevc_slice_rewrite.h"

struct bits {
	uint8_t data[256];
	size_t bit;
};

static void put_bit(struct bits *b, unsigned int value)
{
	if (value & 1)
		b->data[b->bit >> 3] |= 1U << (7 - (b->bit & 7));
	b->bit++;
}

static void put_bits(struct bits *b, unsigned int value, unsigned int count)
{
	int i;

	for (i = (int)count - 1; i >= 0; i--)
		put_bit(b, value >> i);
}

static void put_ue(struct bits *b, unsigned int value)
{
	unsigned int code = value + 1, n = 0, v;

	for (v = code; v; v >>= 1)
		n++;
	put_bits(b, 0, n - 1);
	put_bits(b, code, n);
}

static unsigned int get_bit(const uint8_t *p, size_t *bit)
{
	unsigned int v = (p[*bit >> 3] >> (7 - (*bit & 7))) & 1;
	(*bit)++;
	return v;
}

static unsigned int get_bits(const uint8_t *p, size_t *bit, unsigned int n)
{
	unsigned int i, v = 0;

	for (i = 0; i < n; i++)
		v = (v << 1) | get_bit(p, bit);
	return v;
}

static unsigned int get_ue(const uint8_t *p, size_t *bit)
{
	unsigned int zeros = 0, suffix;

	while (!get_bit(p, bit))
		zeros++;
	suffix = zeros ? get_bits(p, bit, zeros) : 0;
	return ((1U << zeros) - 1) + suffix;
}

static size_t unescape(uint8_t *out, const uint8_t *in, size_t len)
{
	size_t i, n = 0;
	unsigned int zeros = 0;

	out[n++] = in[0];
	out[n++] = in[1];
	for (i = 2; i < len; i++) {
		if (zeros >= 2 && in[i] == 3 && i + 1 < len && in[i + 1] <= 3) {
			zeros = 0;
			continue;
		}
		out[n++] = in[i];
		if (in[i] == 0)
			zeros++;
		else
			zeros = 0;
	}
	return n;
}

static size_t escape(uint8_t *out, const uint8_t *in, size_t len)
{
	size_t i, n = 0;
	unsigned int zeros = 0;

	out[n++] = in[0];
	out[n++] = in[1];
	for (i = 2; i < len; i++) {
		if (zeros >= 2 && in[i] <= 3) {
			out[n++] = 3;
			zeros = 0;
		}
		out[n++] = in[i];
		if (in[i] == 0)
			zeros++;
		else
			zeros = 0;
	}
	return n;
}

static size_t make_sps_index_slice(uint8_t *nal, size_t *header_size)
{
	struct bits b = {0};
	static const uint8_t payload[] = { 0x00, 0x00, 0x01, 0x03, 0xa5, 0x7e };

	put_bits(&b, 0x02, 8); /* TRAIL_R */
	put_bits(&b, 0x01, 8);
	put_bit(&b, 1);        /* first_slice_segment_in_pic_flag */
	put_ue(&b, 5);         /* slice_pic_parameter_set_id */
	put_ue(&b, 1);         /* P slice */
	put_bits(&b, 2, 8);    /* slice_pic_order_cnt_lsb */
	put_bit(&b, 1);        /* short_term_ref_pic_set_sps_flag */
	put_bit(&b, 0);        /* index 0 of two SPS sets */
	put_bits(&b, 5, 3);    /* opaque remainder of slice header */
	put_bit(&b, 1);        /* byte_alignment() */
	while (b.bit & 7)
		put_bit(&b, 0);
	*header_size = b.bit >> 3;
	memcpy(b.data + *header_size, payload, sizeof(payload));
	return escape(nal, b.data, *header_size + sizeof(payload));
}

static void init_picture(VAPictureParameterBufferHEVC *pic)
{
	unsigned int i;

	memset(pic, 0, sizeof(*pic));
	pic->pic_width_in_luma_samples = 1920;
	pic->pic_height_in_luma_samples = 1080;
	pic->log2_min_luma_coding_block_size_minus3 = 0;
	pic->log2_diff_max_min_luma_coding_block_size = 3;
	pic->log2_max_pic_order_cnt_lsb_minus4 = 4;
	pic->num_short_term_ref_pic_sets = 2;
	pic->CurrPic.picture_id = 20;
	pic->CurrPic.pic_order_cnt = 2;
	for (i = 0; i < 15; i++) {
		pic->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
		pic->ReferenceFrames[i].flags = VA_PICTURE_HEVC_INVALID;
	}

	/* Deliberately unordered: the rewriter must emit -1, -2, then +2. */
	pic->ReferenceFrames[0].picture_id = 4;
	pic->ReferenceFrames[0].pic_order_cnt = 4;
	pic->ReferenceFrames[0].flags = VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
	pic->ReferenceFrames[1].picture_id = 0;
	pic->ReferenceFrames[1].pic_order_cnt = 0;
	pic->ReferenceFrames[1].flags = 0; /* StFoll */
	pic->ReferenceFrames[2].picture_id = 1;
	pic->ReferenceFrames[2].pic_order_cnt = 1;
	pic->ReferenceFrames[2].flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE;
}

int main(void)
{
	VAPictureParameterBufferHEVC pic;
	VASliceParameterBufferHEVC slice;
	uint8_t original[256], rewritten[512], rbsp[512];
	size_t original_size, original_header, rbsp_size, bit;
	unsigned int pps_id = 99;
	int rewritten_size, failed = 0;

	init_picture(&pic);
	memset(&slice, 0, sizeof(slice));
	original_size = make_sps_index_slice(original, &original_header);
	slice.slice_data_byte_offset = original_header;
	slice.slice_data_size = original_size;

	rewritten_size = hevc_rewrite_slice(rewritten, sizeof(rewritten),
					    original, original_size,
					    &pic, &slice, &pps_id);
	if (rewritten_size <= 0) {
		fprintf(stderr, "rewrite failed: %d\n", rewritten_size);
		return 1;
	}
	if (pps_id != 5) {
		fprintf(stderr, "PPS id mismatch: %u\n", pps_id);
		failed = 1;
	}

	rbsp_size = unescape(rbsp, rewritten, rewritten_size);
	bit = 16;
	failed |= get_bit(rbsp, &bit) != 1;
	failed |= get_ue(rbsp, &bit) != 5;
	failed |= get_ue(rbsp, &bit) != 1;
	failed |= get_bits(rbsp, &bit, 8) != 2;
	failed |= get_bit(rbsp, &bit) != 0; /* now inline */
	failed |= get_ue(rbsp, &bit) != 2;  /* negative pictures */
	failed |= get_ue(rbsp, &bit) != 1;  /* positive pictures */
	failed |= get_ue(rbsp, &bit) != 0 || get_bit(rbsp, &bit) != 1; /* -1 used */
	failed |= get_ue(rbsp, &bit) != 0 || get_bit(rbsp, &bit) != 0; /* -2 foll */
	failed |= get_ue(rbsp, &bit) != 1 || get_bit(rbsp, &bit) != 1; /* +2 used */
	failed |= get_bits(rbsp, &bit, 3) != 5; /* untouched header suffix */
	failed |= get_bit(rbsp, &bit) != 1;     /* new alignment bit */
	while (bit & 7)
		failed |= get_bit(rbsp, &bit) != 0;
	if (rbsp_size - (bit >> 3) != 6 ||
	    memcmp(rbsp + (bit >> 3), "\x00\x00\x01\x03\xa5\x7e", 6)) {
		fprintf(stderr, "CABAC payload changed\n");
		failed = 1;
	}

	/* No SPS RPS table means no conversion and therefore byte identity. */
	pic.num_short_term_ref_pic_sets = 0;
	rewritten_size = hevc_rewrite_slice(rewritten, sizeof(rewritten),
					    original, original_size,
					    &pic, &slice, &pps_id);
	if (rewritten_size != (int)original_size ||
	    memcmp(rewritten, original, original_size)) {
		fprintf(stderr, "compatible slice was not copied verbatim\n");
		failed = 1;
	}

	/* Do not silently misparse syntax referring to an unavailable SPS LT table. */
	pic.num_short_term_ref_pic_sets = 2;
	pic.slice_parsing_fields.bits.long_term_ref_pics_present_flag = 1;
	pic.num_long_term_ref_pic_sps = 1;
	rewritten_size = hevc_rewrite_slice(rewritten, sizeof(rewritten),
					    original, original_size,
					    &pic, &slice, &pps_id);
	if (rewritten_size != -ENOTSUP) {
		fprintf(stderr, "SPS LT table was not rejected: %d\n", rewritten_size);
		failed = 1;
	}

	if (failed)
		fprintf(stderr, "HEVC slice rewrite validation failed\n");
	return failed;
}
