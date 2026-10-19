// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "hevc_slice_rewrite.h"

struct bit_reader {
	const uint8_t *data;
	size_t size_bits;
	size_t bit;
};

struct bit_writer {
	uint8_t *data;
	size_t size;
	size_t bit;
	int error;
};

struct short_ref {
	int delta_poc;
	int used;
};

static int br_bit(struct bit_reader *r, unsigned int *value)
{
	if (r->bit >= r->size_bits)
		return -EINVAL;
	*value = (r->data[r->bit >> 3] >> (7 - (r->bit & 7))) & 1;
	r->bit++;
	return 0;
}

static int br_bits(struct bit_reader *r, unsigned int count,
		   unsigned int *value)
{
	unsigned int i, bit, v = 0;

	if (count > 32 || r->bit > r->size_bits ||
	    r->size_bits - r->bit < count)
		return -EINVAL;
	for (i = 0; i < count; i++) {
		if (br_bit(r, &bit))
			return -EINVAL;
		v = (v << 1) | bit;
	}
	*value = v;
	return 0;
}

static int br_ue(struct bit_reader *r, unsigned int *value)
{
	unsigned int bit, zeros = 0, suffix = 0;

	for (;;) {
		if (br_bit(r, &bit))
			return -EINVAL;
		if (bit)
			break;
		if (++zeros > 31)
			return -ERANGE;
	}
	if (zeros && br_bits(r, zeros, &suffix))
		return -EINVAL;
	*value = ((1U << zeros) - 1) + suffix;
	return 0;
}

static void bw_bit(struct bit_writer *w, unsigned int value)
{
	size_t byte = w->bit >> 3;

	if (byte >= w->size) {
		w->error = -ENOSPC;
		return;
	}
	if (value & 1)
		w->data[byte] |= 1U << (7 - (w->bit & 7));
	w->bit++;
}

static void bw_bits(struct bit_writer *w, uint64_t value, unsigned int count)
{
	int i;

	for (i = (int)count - 1; i >= 0; i--)
		bw_bit(w, (unsigned int)(value >> i));
}

static void bw_ue(struct bit_writer *w, unsigned int value)
{
	uint64_t code = (uint64_t)value + 1;
	unsigned int bits = 0;
	uint64_t v;

	for (v = code; v; v >>= 1)
		bits++;
	bw_bits(w, 0, bits - 1);
	bw_bits(w, code, bits);
}

static unsigned int ceil_log2(unsigned int value)
{
	unsigned int bits = 0;

	if (value <= 1)
		return 0;
	value--;
	while (value) {
		bits++;
		value >>= 1;
	}
	return bits;
}

static int unescape_nal_prefix(uint8_t *out, size_t out_size,
			       const uint8_t *in, size_t in_size,
			       size_t prefix_size, size_t *in_used)
{
	size_t i, n = 0;
	unsigned int zeros = 0;

	if (in_size < 2 || prefix_size < 2 || out_size < prefix_size)
		return -EINVAL;
	out[n++] = in[0];
	out[n++] = in[1];
	for (i = 2; i < in_size && n < prefix_size; i++) {
		uint8_t v = in[i];

		if (zeros >= 2 && v == 3 && i + 1 < in_size && in[i + 1] <= 3) {
			zeros = 0;
			continue;
		}
		if (n >= out_size)
			return -ENOSPC;
		out[n++] = v;
		if (v == 0)
			zeros++;
		else
			zeros = 0;
	}
	if (n != prefix_size)
		return -EINVAL;
	*in_used = i;
	return 0;
}

static int escape_nal(uint8_t *out, size_t out_size,
		      const uint8_t *in, size_t in_size)
{
	size_t i, n = 0;
	unsigned int zeros = 0;

	if (in_size < 2 || out_size < 2)
		return -EINVAL;
	out[n++] = in[0];
	out[n++] = in[1];
	for (i = 2; i < in_size; i++) {
		uint8_t v = in[i];

		if (zeros >= 2 && v <= 3) {
			if (n >= out_size)
				return -ENOSPC;
			out[n++] = 3;
			zeros = 0;
		}
		if (n >= out_size)
			return -ENOSPC;
		out[n++] = v;
		if (v == 0)
			zeros++;
		else
			zeros = 0;
	}
	return (int)n;
}

static int short_ref_compare(const void *ap, const void *bp)
{
	const struct short_ref *a = ap;
	const struct short_ref *b = bp;

	if (a->delta_poc < 0 && b->delta_poc >= 0)
		return -1;
	if (a->delta_poc >= 0 && b->delta_poc < 0)
		return 1;
	if (a->delta_poc < 0)
		return (b->delta_poc > a->delta_poc) -
		       (b->delta_poc < a->delta_poc);
	return (a->delta_poc > b->delta_poc) -
	       (a->delta_poc < b->delta_poc);
}

static int collect_short_refs(const VAPictureParameterBufferHEVC *pic,
			      struct short_ref refs[15],
			      unsigned int *negative,
			      unsigned int *positive)
{
	unsigned int i, count = 0;

	*negative = 0;
	*positive = 0;
	for (i = 0; i < 15; i++) {
		const VAPictureHEVC *ref = &pic->ReferenceFrames[i];
		uint32_t curr_flags = VA_PICTURE_HEVC_RPS_ST_CURR_BEFORE |
			VA_PICTURE_HEVC_RPS_ST_CURR_AFTER;
		int64_t delta;

		if (ref->picture_id == VA_INVALID_SURFACE ||
		    (ref->flags & VA_PICTURE_HEVC_INVALID) ||
		    (ref->flags & VA_PICTURE_HEVC_LONG_TERM_REFERENCE))
			continue;
		delta = (int64_t)ref->pic_order_cnt - pic->CurrPic.pic_order_cnt;
		if (!delta || delta < INT_MIN || delta > INT_MAX)
			return -EINVAL;
		refs[count].delta_poc = (int)delta;
		refs[count].used = !!(ref->flags & curr_flags);
		if (delta < 0)
			(*negative)++;
		else
			(*positive)++;
		count++;
	}
	qsort(refs, count, sizeof(refs[0]), short_ref_compare);
	return (int)count;
}

static void write_inline_st_rps(struct bit_writer *w,
				const struct short_ref refs[15],
				unsigned int negative,
				unsigned int positive)
{
	unsigned int i;
	int prior = 0;

	/* stRpsIdx is zero in the canonical SPS, so no inter-RPS flag exists. */
	bw_ue(w, negative);
	bw_ue(w, positive);
	for (i = 0; i < negative; i++) {
		unsigned int delta = (unsigned int)(prior - refs[i].delta_poc - 1);

		bw_ue(w, delta);
		bw_bit(w, refs[i].used);
		prior = refs[i].delta_poc;
	}
	prior = 0;
	for (i = negative; i < negative + positive; i++) {
		unsigned int delta = (unsigned int)(refs[i].delta_poc - prior - 1);

		bw_ue(w, delta);
		bw_bit(w, refs[i].used);
		prior = refs[i].delta_poc;
	}
}

static int copy_nal(uint8_t *out, size_t out_size,
		    const uint8_t *nal, size_t nal_size)
{
	if (out_size < nal_size)
		return -ENOSPC;
	memcpy(out, nal, nal_size);
	return (int)nal_size;
}

int hevc_rewrite_slice(uint8_t *out, size_t out_size,
		       const uint8_t *nal, size_t nal_size,
		       const VAPictureParameterBufferHEVC *pic,
		       const VASliceParameterBufferHEVC *slice,
		       unsigned int *pps_id)
{
	struct bit_reader r;
	struct bit_writer w;
	struct short_ref refs[15];
	uint8_t *rbsp = NULL, *new_rbsp = NULL;
	size_t header_bytes, ebsp_header_bytes, new_rbsp_cap;
	size_t syntax_end, rps_start, old_rps_end, i;
	unsigned int value, first, dependent = 0, negative, positive;
	unsigned int nal_type, ctb_log2, width_ctbs, height_ctbs;
	unsigned int address_bits, old_flag;
	int ref_count, ret = -EINVAL;

	if (!out || !nal || !pic || !slice || !pps_id || nal_size < 3)
		return -EINVAL;

	header_bytes = slice->slice_data_byte_offset;
	if (header_bytes < 3 || header_bytes > nal_size ||
	    header_bytes > SIZE_MAX - 256)
		return -EINVAL;
	new_rbsp_cap = header_bytes + 256;
	if (new_rbsp_cap > out_size)
		new_rbsp_cap = out_size;
	rbsp = malloc(header_bytes);
	new_rbsp = calloc(1, new_rbsp_cap);
	if (!rbsp || !new_rbsp) {
		ret = -ENOMEM;
		goto out;
	}
	ret = unescape_nal_prefix(rbsp, header_bytes, nal, nal_size,
				  header_bytes, &ebsp_header_bytes);
	if (ret < 0)
		goto out;
	r.data = rbsp;
	r.size_bits = header_bytes * 8;
	r.bit = 16;
	nal_type = (nal[0] >> 1) & 0x3f;

	if (br_bit(&r, &first))
		goto out;
	if (nal_type >= 16 && nal_type <= 23 && br_bit(&r, &value))
		goto out;
	if (br_ue(&r, pps_id) || *pps_id > 63)
		goto out;
	if (!first) {
		if (pic->slice_parsing_fields.bits.dependent_slice_segments_enabled_flag &&
		    br_bit(&r, &dependent))
			goto out;
		ctb_log2 = pic->log2_min_luma_coding_block_size_minus3 + 3 +
			pic->log2_diff_max_min_luma_coding_block_size;
		if (ctb_log2 >= 31)
			goto out;
		width_ctbs = (pic->pic_width_in_luma_samples +
			       (1U << ctb_log2) - 1) >> ctb_log2;
		height_ctbs = (pic->pic_height_in_luma_samples +
				(1U << ctb_log2) - 1) >> ctb_log2;
		if (!width_ctbs || !height_ctbs ||
		    width_ctbs > UINT_MAX / height_ctbs)
			goto out;
		address_bits = ceil_log2(width_ctbs * height_ctbs);
		if (br_bits(&r, address_bits, &value))
			goto out;
	}

	if (dependent) {
		ret = copy_nal(out, out_size, nal, nal_size);
		goto out;
	}
	if (br_bits(&r, pic->num_extra_slice_header_bits, &value) ||
	    br_ue(&r, &value))
		goto out;
	if (pic->slice_parsing_fields.bits.output_flag_present_flag &&
	    br_bit(&r, &value))
		goto out;
	if (pic->pic_fields.bits.separate_colour_plane_flag &&
	    br_bits(&r, 2, &value))
		goto out;

	if (nal_type == 19 || nal_type == 20) {
		ret = copy_nal(out, out_size, nal, nal_size);
		goto out;
	}
	if (br_bits(&r, pic->log2_max_pic_order_cnt_lsb_minus4 + 4, &value))
		goto out;

	rps_start = r.bit;
	if (br_bit(&r, &old_flag))
		goto out;
	if (!old_flag) {
		if ((size_t)pic->st_rps_bits > r.size_bits - r.bit)
			goto out;
		r.bit += pic->st_rps_bits;
	} else if (pic->num_short_term_ref_pic_sets > 1) {
		if (br_bits(&r, ceil_log2(pic->num_short_term_ref_pic_sets), &value))
			goto out;
	}
	old_rps_end = r.bit;

	if (pic->slice_parsing_fields.bits.long_term_ref_pics_present_flag &&
	    pic->num_long_term_ref_pic_sps) {
		ret = -ENOTSUP;
		goto out;
	}

	if (!pic->num_short_term_ref_pic_sets) {
		ret = copy_nal(out, out_size, nal, nal_size);
		goto out;
	}

	if (rbsp[header_bytes - 1] == 0)
		goto out;
	syntax_end = header_bytes * 8 -
		((unsigned int)__builtin_ctz((unsigned int)rbsp[header_bytes - 1]) + 1);
	if (old_rps_end > syntax_end)
		goto out;

	ref_count = collect_short_refs(pic, refs, &negative, &positive);
	if (ref_count < 0) {
		ret = ref_count;
		goto out;
	}

	w.data = new_rbsp;
	w.size = new_rbsp_cap;
	w.bit = 0;
	w.error = 0;
	for (i = 0; i < rps_start; i++)
		bw_bit(&w, (rbsp[i >> 3] >> (7 - (i & 7))) & 1);
	bw_bit(&w, 0);
	write_inline_st_rps(&w, refs, negative, positive);
	for (i = old_rps_end; i < syntax_end; i++)
		bw_bit(&w, (rbsp[i >> 3] >> (7 - (i & 7))) & 1);
	bw_bit(&w, 1);
	while (w.bit & 7)
		bw_bit(&w, 0);
	if (w.error) {
		ret = w.error;
		goto out;
	}
	/* Both the old and new slice headers end with a non-zero alignment byte,
	 * so emulation-prevention state is reset at this boundary.  Escape only
	 * the rewritten header and append the original EBSP CABAC payload. */
	ret = escape_nal(out, out_size, new_rbsp, w.bit >> 3);
	if (ret >= 0) {
		size_t payload_size = nal_size - ebsp_header_bytes;

		if ((size_t)ret > out_size ||
		    payload_size > out_size - (size_t)ret) {
			ret = -ENOSPC;
			goto out;
		}
		if (payload_size > (size_t)(INT_MAX - ret)) {
			ret = -EOVERFLOW;
			goto out;
		}
		memcpy(out + ret, nal + ebsp_header_bytes, payload_size);
		ret += (int)payload_size;
	}

out:
	free(new_rbsp);
	free(rbsp);
	return ret;
}
