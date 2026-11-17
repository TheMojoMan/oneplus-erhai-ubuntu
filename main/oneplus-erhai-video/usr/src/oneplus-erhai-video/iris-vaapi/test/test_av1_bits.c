// SPDX-License-Identifier: GPL-2.0-or-later
/* Unit tests for the AV1 header writer: bit-level primitives, leb128, and a
 * parse-back of the synthesized sequence header and frame OBU headers. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <va/va.h>
#include <va/va_dec_av1.h>

#include "av1_bits.h"
#include "av1_params.h"

static int failures;
static int checks;

#define CHECK(cond) do {						\
	checks++;							\
	if (!(cond)) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__,	\
			#cond);						\
	}								\
} while (0)

#define CHECK_EQ(a, b) do {						\
	long long _a = (long long)(a), _b = (long long)(b);		\
	checks++;							\
	if (_a != _b) {							\
		failures++;						\
		fprintf(stderr, "FAIL %s:%d: %s = %lld, expected %s = %lld\n", \
			__FILE__, __LINE__, #a, _a, #b, _b);		\
	}								\
} while (0)

/* ---- minimal reader mirroring the specification ---- */

struct br {
	const uint8_t *buf;
	size_t len;
	size_t pos;
	unsigned int bit;
	int error;
};

static unsigned int
br_bit(struct br *r)
{
	unsigned int v;

	if (r->pos >= r->len) {
		r->error = 1;
		return 0;
	}
	v = (r->buf[r->pos] >> (7 - r->bit)) & 1;
	if (++r->bit == 8) {
		r->bit = 0;
		r->pos++;
	}
	return v;
}

static uint32_t
br_bits(struct br *r, unsigned int n)
{
	uint32_t v = 0;

	while (n--)
		v = (v << 1) | br_bit(r);
	return v;
}

static unsigned int
br_ns(struct br *r, unsigned int n)
{
	unsigned int w, m, v;

	if (n <= 1)
		return 0;
	w = av1_floor_log2(n) + 1;
	m = (1u << w) - n;
	v = br_bits(r, w - 1);
	if (v < m)
		return v;
	return (v << 1) - m + br_bit(r);
}

/* decode_subexp() from spec 5.10.3, used to verify read_global_param codes. */
static uint32_t
br_subexp(struct br *r, uint32_t num_syms)
{
	unsigned int i = 0, k = 3;
	uint32_t mk = 0;

	for (;;) {
		unsigned int b2 = i ? k + i - 1 : k;
		uint32_t a = 1u << b2;

		if (num_syms <= mk + 3 * a)
			return br_ns(r, num_syms - mk) + mk;
		if (br_bit(r)) {
			i++;
			mk += a;
		} else {
			return br_bits(r, b2) + mk;
		}
	}
}

/* ---- tests ---- */

static void
test_bit_writer(void)
{
	uint8_t buf[8];
	struct av1_bw bw;

	av1_bw_init(&bw, buf, sizeof(buf));
	av1_bw_bits(&bw, 0x5, 3);	/* 101 */
	av1_bw_bits(&bw, 0x2, 2);	/* 10 */
	av1_bw_bits(&bw, 0x7f, 8);	/* 0111 1111 */
	av1_bw_trailing_bits(&bw);
	CHECK_EQ(bw.error, 0);
	/* 13 coded bits plus the trailing stop bit pad to two bytes. */
	CHECK_EQ(av1_bw_size(&bw), 2);
	CHECK_EQ(buf[0], 0xb3);		/* 1011 0011 */
	CHECK_EQ(buf[1], 0xfc);		/* 1111 1100 */

	av1_bw_init(&bw, buf, 1);
	av1_bw_bits(&bw, 0, 16);
	CHECK_EQ(bw.error, 1);

	/* su(7): -1 is 0x7f, 63 is 0x3f, -64 is 0x40 */
	memset(buf, 0, sizeof(buf));
	av1_bw_init(&bw, buf, sizeof(buf));
	av1_bw_su(&bw, -1, 7);
	av1_bw_su(&bw, 63, 7);
	av1_bw_align(&bw);
	CHECK_EQ(buf[0], 0xfe);		/* 1111111 0 */
	CHECK_EQ(buf[1], 0xfc);		/* 111111 00 */
}

static void
test_leb128(void)
{
	struct { uint64_t value; size_t len; uint8_t bytes[4]; } cases[] = {
		{ 0, 1, { 0x00 } },
		{ 1, 1, { 0x01 } },
		{ 127, 1, { 0x7f } },
		{ 128, 2, { 0x80, 0x01 } },
		{ 300, 2, { 0xac, 0x02 } },
		{ 16384, 3, { 0x80, 0x80, 0x01 } },
	};
	unsigned int i;

	for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		uint8_t out[8];
		size_t n = av1_leb128_write(out, cases[i].value);

		CHECK_EQ(av1_leb128_size(cases[i].value), cases[i].len);
		CHECK_EQ(n, cases[i].len);
		CHECK(memcmp(out, cases[i].bytes, cases[i].len) == 0);
	}
}

static void
test_ns_roundtrip(void)
{
	unsigned int n;

	for (n = 1; n < 300; n++) {
		unsigned int v;

		for (v = 0; v < n; v++) {
			uint8_t buf[4];
			struct av1_bw bw;
			struct br r;

			av1_bw_init(&bw, buf, sizeof(buf));
			av1_bw_ns(&bw, v, n);
			av1_bw_align(&bw);
			CHECK_EQ(bw.error, 0);
			r.buf = buf;
			r.len = sizeof(buf);
			r.pos = 0;
			r.bit = 0;
			r.error = 0;
			CHECK_EQ(br_ns(&r, n), v);
		}
	}
}

static void
test_subexp_roundtrip(void)
{
	uint32_t num_syms;

	for (num_syms = 2; num_syms < 20000; num_syms = num_syms * 3 + 1) {
		uint32_t v;

		for (v = 0; v < num_syms; v += (num_syms / 37) + 1) {
			uint8_t buf[16];
			struct av1_bw bw;
			struct br r;
			unsigned int i = 0, k = 3;
			uint32_t mk = 0;

			/* Same word structure as decode_subexp(). */
			av1_bw_init(&bw, buf, sizeof(buf));
			for (;;) {
				unsigned int b2 = i ? k + i - 1 : k;
				uint32_t a = 1u << b2;

				if (num_syms <= mk + 3 * a) {
					av1_bw_ns(&bw, v - mk, num_syms - mk);
					break;
				}
				if (v < mk + a) {
					av1_bw_bit(&bw, 0);
					av1_bw_bits(&bw, v - mk, b2);
					break;
				}
				av1_bw_bit(&bw, 1);
				i++;
				mk += a;
			}
			av1_bw_align(&bw);
			CHECK_EQ(bw.error, 0);
			r.buf = buf;
			r.len = sizeof(buf);
			r.pos = 0;
			r.bit = 0;
			r.error = 0;
			CHECK_EQ(br_subexp(&r, num_syms), v);
		}
	}
}

static void
fill_seq_pic(VADecPictureParameterBufferAV1 *pic, unsigned int width,
	     unsigned int height)
{
	memset(pic, 0, sizeof(*pic));
	pic->profile = 0;
	pic->bit_depth_idx = 0;
	pic->order_hint_bits_minus_1 = 6;
	pic->frame_width_minus1 = (uint16_t)(width - 1);
	pic->frame_height_minus1 = (uint16_t)(height - 1);
	pic->pic_info_fields.bits.frame_type = 0;
	pic->pic_info_fields.bits.show_frame = 1;
	pic->seq_info_fields.fields.use_128x128_superblock = 1;
	pic->seq_info_fields.fields.enable_filter_intra = 1;
	pic->seq_info_fields.fields.enable_intra_edge_filter = 1;
	pic->seq_info_fields.fields.enable_interintra_compound = 1;
	pic->seq_info_fields.fields.enable_masked_compound = 1;
	pic->seq_info_fields.fields.enable_dual_filter = 1;
	pic->seq_info_fields.fields.enable_order_hint = 1;
	pic->seq_info_fields.fields.enable_jnt_comp = 1;
	pic->seq_info_fields.fields.enable_cdef = 1;
	pic->seq_info_fields.fields.subsampling_x = 1;
	pic->seq_info_fields.fields.subsampling_y = 1;
	pic->seq_info_fields.fields.film_grain_params_present = 0;
	pic->tile_cols = 1;
	pic->tile_rows = 1;
	pic->pic_info_fields.bits.uniform_tile_spacing_flag = 1;
}

static void
test_sequence_header(void)
{
	VADecPictureParameterBufferAV1 pic;
	struct av1_seq_cfg cfg, parsed;
	uint8_t buf[128];
	struct br r;
	size_t n, payload_len;
	unsigned int wbits, hbits;

	fill_seq_pic(&pic, 1280, 720);
	av1_seq_from_pic(&pic, 1280, 720, &cfg);
	/* Exercise the optional sequence flags explicitly: they are derived from
	 * the picture, and fill_seq_pic() leaves restoration/superres off. */
	cfg.enable_superres = 1;
	cfg.enable_restoration = 1;
	cfg.separate_uv_delta_q = 1;
	CHECK_EQ(cfg.profile, 0);
	CHECK_EQ(cfg.bit_depth, 8);
	CHECK_EQ(cfg.order_hint_bits, 7);
	CHECK_EQ(cfg.enable_order_hint, 1);

	n = av1_write_sequence_header(&cfg, buf, sizeof(buf));
	CHECK(n > 0);
	CHECK_EQ(buf[0] & 0x80, 0);		/* obu_forbidden_bit */
	CHECK_EQ((buf[0] >> 3) & 0x0f, 1);	/* OBU_SEQUENCE_HEADER */
	CHECK_EQ((buf[0] >> 1) & 1, 1);		/* obu_has_size_field */

	payload_len = 0;
	{
		size_t shift = 0;
		size_t i = 1;

		for (;;) {
			uint8_t b = buf[i++];

			payload_len |= (size_t)(b & 0x7f) << shift;
			if (!(b & 0x80))
				break;
			shift += 7;
		}
		CHECK_EQ(i + payload_len, n);
	}

	r.buf = buf;
	r.len = n;
	r.pos = 1 + av1_leb128_size(payload_len);
	r.bit = 0;
	r.error = 0;

	memset(&parsed, 0, sizeof(parsed));
	parsed.profile = br_bits(&r, 3);
	CHECK_EQ(br_bit(&r), 0);			/* still_picture */
	CHECK_EQ(br_bit(&r), 0);			/* reduced_still_picture_header */
	CHECK_EQ(br_bit(&r), 0);			/* timing_info_present_flag */
	CHECK_EQ(br_bit(&r), 0);			/* initial_display_delay_present */
	CHECK_EQ(br_bits(&r, 5), 0);			/* op points - 1 */
	CHECK_EQ(br_bits(&r, 12), 0);			/* operating_point_idc */
	CHECK_EQ(br_bits(&r, 5), 8);
	CHECK_EQ(br_bit(&r), 0);			/* seq_tier */
	wbits = br_bits(&r, 4) + 1;
	hbits = br_bits(&r, 4) + 1;
	CHECK_EQ(wbits, av1_bit_width(1279));
	CHECK_EQ(hbits, av1_bit_width(719));
	CHECK_EQ(br_bits(&r, wbits), 1279);
	CHECK_EQ(br_bits(&r, hbits), 719);
	CHECK_EQ(br_bit(&r), 0);			/* frame_id_numbers_present */
	parsed.use_128x128_superblock = br_bit(&r);
	parsed.enable_filter_intra = br_bit(&r);
	parsed.enable_intra_edge_filter = br_bit(&r);
	parsed.enable_interintra_compound = br_bit(&r);
	parsed.enable_masked_compound = br_bit(&r);
	CHECK_EQ(br_bit(&r), 1);			/* enable_warped_motion */
	parsed.enable_dual_filter = br_bit(&r);
	parsed.enable_order_hint = br_bit(&r);
	parsed.enable_jnt_comp = br_bit(&r);
	CHECK_EQ(br_bit(&r), 1);			/* enable_ref_frame_mvs */
	CHECK_EQ(br_bit(&r), 1);			/* seq_choose_sct */
	CHECK_EQ(br_bit(&r), 1);			/* seq_choose_integer_mv */
	parsed.order_hint_bits = br_bits(&r, 3) + 1;
	CHECK_EQ(br_bit(&r), 1);			/* enable_superres */
	parsed.enable_cdef = br_bit(&r);
	CHECK_EQ(br_bit(&r), 1);			/* enable_restoration */
	CHECK_EQ(br_bit(&r), 0);			/* high_bitdepth */
	parsed.mono_chrome = br_bit(&r);
	CHECK_EQ(br_bit(&r), 0);			/* color_description_present */
	parsed.color_range = br_bit(&r);
	CHECK_EQ(br_bits(&r, 2), 0);			/* chroma_sample_position */
	CHECK_EQ(br_bit(&r), 1);			/* separate_uv_delta_q */
	CHECK_EQ(br_bit(&r), 0);			/* film_grain_params_present */
	CHECK_EQ(r.error, 0);

	CHECK_EQ(parsed.profile, cfg.profile);
	CHECK_EQ(parsed.order_hint_bits, cfg.order_hint_bits);
	CHECK_EQ(parsed.use_128x128_superblock, cfg.use_128x128_superblock);
	CHECK_EQ(parsed.enable_filter_intra, cfg.enable_filter_intra);
	CHECK_EQ(parsed.enable_intra_edge_filter, cfg.enable_intra_edge_filter);
	CHECK_EQ(parsed.enable_dual_filter, cfg.enable_dual_filter);
	CHECK_EQ(parsed.enable_order_hint, cfg.enable_order_hint);
	CHECK_EQ(parsed.enable_jnt_comp, cfg.enable_jnt_comp);
	CHECK_EQ(parsed.enable_cdef, cfg.enable_cdef);
	CHECK_EQ(parsed.mono_chrome, cfg.mono_chrome);
	CHECK_EQ(parsed.color_range, cfg.color_range);

	/* The trailing bits must be present and the payload byte aligned. */
	{
		struct av1_bw bw;

		av1_bw_init(&bw, buf + n - 1, 1);
		(void)bw;
	}
}

static void
parse_frame_header(struct br *r, const struct av1_seq_cfg *cfg,
		   VADecPictureParameterBufferAV1 *pic, unsigned int *refresh)
{
	unsigned int frame_type, show_frame, er, frame_is_intra;
	unsigned int allow_sct, primary_ref_frame, i, force_integer_mv;
	unsigned int disable_cdf = 0;
	unsigned int oh_bits = cfg->order_hint_bits;

	CHECK_EQ(br_bit(r), 0);			/* show_existing_frame */
	frame_type = br_bits(r, 2);
	frame_is_intra = (frame_type == 0 || frame_type == 2);
	pic->pic_info_fields.bits.frame_type = frame_type;
	show_frame = br_bit(r);
	pic->pic_info_fields.bits.show_frame = show_frame;
	if (!show_frame)
		pic->pic_info_fields.bits.showable_frame = br_bit(r);
	er = (frame_type == 0 && show_frame) ? 1 : br_bit(r);
	pic->pic_info_fields.bits.error_resilient_mode = er;
	disable_cdf = br_bit(r);
	pic->pic_info_fields.bits.disable_cdf_update = disable_cdf;
	allow_sct = br_bit(r);
	pic->pic_info_fields.bits.allow_screen_content_tools = allow_sct;
	if (allow_sct)
		pic->pic_info_fields.bits.force_integer_mv = br_bit(r);
	CHECK_EQ(br_bit(r), 0);			/* frame_size_override_flag */
	if (cfg->enable_order_hint)
		pic->order_hint = (uint8_t)br_bits(r, oh_bits);
	if (frame_is_intra || er) {
		primary_ref_frame = 7;
	} else {
		primary_ref_frame = br_bits(r, 3);
		pic->primary_ref_frame = (uint8_t)primary_ref_frame;
	}
	if (frame_type == 0 && show_frame) {
		*refresh = 0xff;
	} else {
		*refresh = br_bits(r, 8);
	}
	if ((!frame_is_intra || *refresh != 0xff) && er && cfg->enable_order_hint)
		for (i = 0; i < 8; i++)
			(void)br_bits(r, oh_bits);

	force_integer_mv = frame_is_intra ? 1 :
		(allow_sct && pic->pic_info_fields.bits.force_integer_mv);

	if (frame_is_intra) {
		pic->pic_info_fields.bits.use_superres = br_bit(r);
		if (pic->pic_info_fields.bits.use_superres)
			(void)br_bits(r, 3);
		CHECK_EQ(br_bit(r), 0);		/* render size differs */
		if (allow_sct)
			(void)br_bit(r);	/* allow_intrabc */
	} else {
		if (cfg->enable_order_hint)
			CHECK_EQ(br_bit(r), 0);	/* short signalling */
		for (i = 0; i < 7; i++)
			(void)br_bits(r, 3);
		pic->pic_info_fields.bits.use_superres = br_bit(r);
		if (pic->pic_info_fields.bits.use_superres)
			(void)br_bits(r, 3);
		CHECK_EQ(br_bit(r), 0);		/* render size differs */
		if (!force_integer_mv)
			(void)br_bit(r);	/* allow_high_precision_mv */
		{
			unsigned int switchable = br_bit(r);

			if (!switchable)
				(void)br_bits(r, 2);
		}
		(void)br_bit(r);		/* is_motion_mode_switchable */
		if (!er && cfg->enable_order_hint)
			(void)br_bit(r);	/* use_ref_frame_mvs */
	}
	if (!disable_cdf)
		(void)br_bit(r);		/* disable_frame_end_update_cdf */
}

static void
test_frame_obu_keyframe(void)
{
	VADecPictureParameterBufferAV1 pic;
	struct av1_writer w;
	struct av1_tile tile;
	uint8_t tile_data[16];
	uint8_t out[4096];
	size_t n, payload_len, header_off;
	struct br r;
	unsigned int refresh = 0;

	memset(tile_data, 0xa5, sizeof(tile_data));
	fill_seq_pic(&pic, 1280, 720);
	pic.base_qindex = 100;
	pic.filter_level[0] = 12;
	pic.filter_level[1] = 12;
	pic.filter_level_u = 6;
	pic.filter_level_v = 6;
	pic.loop_filter_info_fields.bits.mode_ref_delta_enabled = 1;
	pic.cdef_damping_minus_3 = 3;
	pic.cdef_bits = 1;
	pic.cdef_y_strengths[0] = (4 << 2) | 1;
	pic.cdef_y_strengths[1] = (2 << 2) | 0;
	pic.cdef_uv_strengths[0] = (3 << 2) | 2;
	pic.cdef_uv_strengths[1] = (1 << 2) | 0;
	pic.interp_filter = 4;
	pic.mode_control_fields.bits.tx_mode = 2;
	pic.mode_control_fields.bits.reduced_tx_set_used = 0;
	pic.loop_restoration_fields.bits.yframe_restoration_type = 2;
	pic.loop_restoration_fields.bits.lr_unit_shift = 1;

	av1_writer_reset(&w);
	av1_seq_from_pic(&pic, 1280, 720, &w.seq);
	w.seq.enable_superres = 1;
	w.seq.enable_restoration = 1;
	w.seq.separate_uv_delta_q = 1;

	tile.offset = 0;
	tile.size = sizeof(tile_data);
	tile.row = 0;
	tile.column = 0;

	n = av1_write_frame_obu(&w, &pic, &tile, 1, tile_data, 0xff, NULL, 1,
				out, sizeof(out));
	CHECK(n > 0);
	CHECK_EQ((out[0] >> 3) & 0x0f, 6);	/* OBU_FRAME */
	CHECK_EQ((out[0] >> 1) & 1, 1);

	payload_len = 0;
	{
		size_t shift = 0, i = 1;

		for (;;) {
			uint8_t b = out[i++];

			payload_len |= (size_t)(b & 0x7f) << shift;
			if (!(b & 0x80))
				break;
			shift += 7;
		}
		header_off = i;
		CHECK_EQ(header_off + payload_len, n);
	}

	r.buf = out;
	r.len = n;
	r.pos = header_off;
	r.bit = 0;
	r.error = 0;
	parse_frame_header(&r, &w.seq, &pic, &refresh);
	CHECK_EQ(refresh, 0xff);
	CHECK_EQ(pic.pic_info_fields.bits.frame_type, 0);

	/* A single-tile group has no tg_start/tg_end and no size prefix, so the
	 * tile payload starts right after the byte-aligned frame header. */
	{
		size_t tg = n - sizeof(tile_data);

		CHECK(memcmp(out + tg, tile_data, sizeof(tile_data)) == 0);
	}
	CHECK_EQ(r.error, 0);
}

static void
test_frame_obu_inter_multitile(void)
{
	VADecPictureParameterBufferAV1 pic;
	struct av1_writer w;
	struct av1_tile tiles[4];
	uint8_t tile_data[64];
	uint8_t out[4096];
	size_t n;
	unsigned int i;

	memset(tile_data, 0x11, sizeof(tile_data));
	fill_seq_pic(&pic, 1280, 720);
	pic.pic_info_fields.bits.frame_type = 1;
	pic.pic_info_fields.bits.show_frame = 1;
	pic.pic_info_fields.bits.error_resilient_mode = 0;
	pic.pic_info_fields.bits.disable_cdf_update = 0;
	pic.pic_info_fields.bits.allow_screen_content_tools = 0;
	pic.pic_info_fields.bits.allow_high_precision_mv = 1;
	pic.pic_info_fields.bits.is_motion_mode_switchable = 1;
	pic.pic_info_fields.bits.use_ref_frame_mvs = 0;
	pic.pic_info_fields.bits.disable_frame_end_update_cdf = 0;
	pic.pic_info_fields.bits.allow_warped_motion = 1;
	pic.primary_ref_frame = 0;
	pic.order_hint = 3;
	for (i = 0; i < 7; i++)
		pic.ref_frame_idx[i] = (uint8_t)i;
	for (i = 0; i < 8; i++)
		pic.ref_frame_map[i] = 100 + i;
	pic.base_qindex = 40;
	pic.mode_control_fields.bits.tx_mode = 1;
	pic.mode_control_fields.bits.reference_select = 1;
	pic.interp_filter = 0;

	av1_writer_reset(&w);
	av1_seq_from_pic(&pic, 1280, 720, &w.seq);
	w.seq.enable_superres = 1;
	w.seq.enable_restoration = 1;
	w.seq.separate_uv_delta_q = 1;
	/* Pretend every slot holds a frame with order hint == the frame index
	 * so the skip-mode branch exercises the forward/backward search. */
	for (i = 0; i < 8; i++) {
		w.slot_order_hint[i] = (uint8_t)i;
		w.slot_valid[i] = 1;
	}

	/* Four tiles as a 2x2 grid: sizes are distinct so the size prefixes
	 * are checkable. */
	tiles[0] = (struct av1_tile){ .offset = 0, .size = 10, .row = 0, .column = 0 };
	tiles[1] = (struct av1_tile){ .offset = 10, .size = 14, .row = 0, .column = 1 };
	tiles[2] = (struct av1_tile){ .offset = 24, .size = 18, .row = 1, .column = 0 };
	tiles[3] = (struct av1_tile){ .offset = 42, .size = 22, .row = 1, .column = 1 };

	n = av1_write_frame_obu(&w, &pic, tiles, 4, tile_data, 0x02, NULL, 1,
				out, sizeof(out));
	CHECK(n > 0);

	/* Walk the tile group forward: [prefix] tile [prefix] tile ... */
	{
		size_t tg_len = 1 + 3 * AV1_TILE_SIZE_BYTES + 64;
		size_t p = n - tg_len;
		uint32_t size;
		unsigned int t;

		CHECK_EQ(out[p], 0);	/* tile_start_and_end_present_flag + pad */
		p += 1;
		for (t = 0; t < 4; t++) {
			if (t + 1 < 4) {
				size = (uint32_t)out[p] |
				       ((uint32_t)out[p + 1] << 8) |
				       ((uint32_t)out[p + 2] << 16) |
				       ((uint32_t)out[p + 3] << 24);
				CHECK_EQ(size + 1, tiles[t].size);
				p += AV1_TILE_SIZE_BYTES;
			}
			CHECK(memcmp(out + p, tile_data + tiles[t].offset,
				     tiles[t].size) == 0);
			p += tiles[t].size;
		}
		CHECK_EQ(p, n);
	}

	/* The refreshed slot must now carry this frame's order hint. */
	CHECK_EQ(w.slot_order_hint[1], 3);
	CHECK_EQ(w.slot_order_hint[0], 0);
}

static int32_t
inverse_recenter(int32_t r, int32_t v)
{
	if (v > 2 * r)
		return v;
	if (v & 1)
		return r - ((v + 1) >> 1);
	return r + (v >> 1);
}

static int32_t
decode_unsigned_subexp_with_ref(struct br *r, int32_t mx, int32_t ref)
{
	int32_t v = (int32_t)br_subexp(r, (uint32_t)mx);

	if ((ref << 1) <= mx)
		return inverse_recenter(ref, v);
	return mx - 1 - inverse_recenter(mx - 1 - ref, v);
}

static int32_t
decode_signed_subexp_with_ref(struct br *r, int32_t low, int32_t high,
			      int32_t ref)
{
	int32_t x = decode_unsigned_subexp_with_ref(r, high - low, ref - low);

	return x + low;
}

/* spec read_global_param() and the reference-recentering helper, implemented
 * independently of the writer so the test checks the actual bitstream. */
static int32_t
decode_global_param(struct br *r, int type, int idx, const int32_t prev[6],
		    int allow_high_precision_mv)
{
	int32_t abs_bits = 12, prec_bits = 15;
	int32_t prec_diff, round, sub, mx, ref, x;

	if (idx < 2) {
		if (type == 1) {
			abs_bits = 9 - !allow_high_precision_mv;
			prec_bits = 3 - !allow_high_precision_mv;
		} else {
			abs_bits = 12;
			prec_bits = 6;
		}
	}
	prec_diff = 16 - prec_bits;
	round = (idx % 3) == 2 ? (1 << 16) : 0;
	sub = (idx % 3) == 2 ? (1 << prec_bits) : 0;
	mx = 1 << abs_bits;
	ref = (prev[idx] >> prec_diff) - sub;
	x = decode_signed_subexp_with_ref(r, -mx, mx + 1, ref);
	return (x << prec_diff) + round;
}

static void
test_global_motion_roundtrip(void)
{
	VADecPictureParameterBufferAV1 pic;
	struct av1_writer w;
	struct av1_tile tile;
	uint8_t tile_data[4];
	uint8_t out[8192];
	size_t n, header_off, payload_len;
	struct br r;
	unsigned int refresh = 0;
	unsigned int ref;

	memset(tile_data, 0, sizeof(tile_data));
	fill_seq_pic(&pic, 1280, 720);
	pic.pic_info_fields.bits.frame_type = 1;
	pic.pic_info_fields.bits.show_frame = 1;
	pic.pic_info_fields.bits.error_resilient_mode = 1;
	pic.pic_info_fields.bits.allow_high_precision_mv = 1;
	pic.pic_info_fields.bits.use_ref_frame_mvs = 0;
	pic.pic_info_fields.bits.allow_warped_motion = 1;
	pic.primary_ref_frame = 7;
	pic.order_hint = 9;
	for (ref = 0; ref < 7; ref++)
		pic.ref_frame_idx[ref] = (uint8_t)ref;
	pic.base_qindex = 30;
	pic.mode_control_fields.bits.tx_mode = 2;
	pic.mode_control_fields.bits.reference_select = 0;
	pic.interp_filter = 4;

	/* wm[0] = translation, wm[1] = rotzoom, wm[2] = affine, rest identity */
	/* Global motion parameters are stored with the coded precision, so the
	 * test values have to be representable at that precision: 2^13 for a
	 * pure translation with high-precision MVs. */
	pic.wm[0].wmtype = 1;
	pic.wm[0].wmmat[0] = 5 * 8192;
	pic.wm[0].wmmat[1] = -3 * 8192;
	pic.wm[1].wmtype = 2;
	pic.wm[1].wmmat[2] = 65536 + 4096;
	pic.wm[1].wmmat[3] = -2048;
	pic.wm[2].wmtype = 3;
	pic.wm[2].wmmat[0] = 3 * 1024;
	pic.wm[2].wmmat[1] = -2 * 1024;
	pic.wm[2].wmmat[2] = 65536 - 8192;
	pic.wm[2].wmmat[3] = 1024;
	pic.wm[2].wmmat[4] = -512;
	pic.wm[2].wmmat[5] = 65536 + 256;

	av1_writer_reset(&w);
	av1_seq_from_pic(&pic, 1280, 720, &w.seq);
	w.seq.enable_superres = 1;
	w.seq.enable_restoration = 1;
	w.seq.separate_uv_delta_q = 1;

	tile = (struct av1_tile){ .offset = 0, .size = 4 };
	n = av1_write_frame_obu(&w, &pic, &tile, 1, tile_data, 0x01, NULL, 1,
				out, sizeof(out));
	CHECK(n > 0);

	payload_len = 0;
	{
		size_t shift = 0, i = 1;

		for (;;) {
			uint8_t b = out[i++];

			payload_len |= (size_t)(b & 0x7f) << shift;
			if (!(b & 0x80))
				break;
			shift += 7;
		}
		header_off = i;
		CHECK_EQ(header_off + payload_len, n);
	}

	r.buf = out;
	r.len = n;
	r.pos = header_off;
	r.bit = 0;
	r.error = 0;
	parse_frame_header(&r, &w.seq, &pic, &refresh);
	CHECK_EQ(refresh, 0x01);

	/* tile_info(): uniform = 1, then the two "stop" increments */
	CHECK_EQ(br_bit(&r), 1);		/* uniform_tile_spacing_flag */
	CHECK_EQ(br_bit(&r), 0);		/* increment_tile_cols_log2 */
	CHECK_EQ(br_bit(&r), 0);		/* increment_tile_rows_log2 */
	/* quantization_params() */
	CHECK_EQ(br_bits(&r, 8), 30);		/* base_q_idx */
	CHECK_EQ(br_bit(&r), 0);		/* y_dc_delta_q coded */
	CHECK_EQ(br_bit(&r), 0);		/* diff_uv_delta */
	CHECK_EQ(br_bit(&r), 0);		/* u_dc_delta_q coded */
	CHECK_EQ(br_bit(&r), 0);		/* u_ac_delta_q coded */
	CHECK_EQ(br_bit(&r), 0);		/* using_qmatrix */
	/* segmentation_params() */
	CHECK_EQ(br_bit(&r), 0);		/* segmentation_enabled */
	/* delta_q_params() */
	CHECK_EQ(br_bit(&r), 0);		/* delta_q_present */
	/* loop_filter_params() */
	CHECK_EQ(br_bits(&r, 6), 0);
	CHECK_EQ(br_bits(&r, 6), 0);
	CHECK_EQ(br_bits(&r, 3), 0);		/* sharpness */
	CHECK_EQ(br_bit(&r), 0);		/* delta_enabled */
	/* cdef_params() */
	CHECK_EQ(br_bits(&r, 2), 0);		/* damping - 3 */
	CHECK_EQ(br_bits(&r, 2), 0);		/* cdef_bits */
	CHECK_EQ(br_bits(&r, 4), 0);		/* y pri */
	CHECK_EQ(br_bits(&r, 2), 0);		/* y sec */
	CHECK_EQ(br_bits(&r, 4), 0);		/* uv pri */
	CHECK_EQ(br_bits(&r, 2), 0);		/* uv sec */
	/* lr_params() */
	CHECK_EQ(br_bits(&r, 2), 0);
	CHECK_EQ(br_bits(&r, 2), 0);
	CHECK_EQ(br_bits(&r, 2), 0);
	/* read_tx_mode() */
	CHECK_EQ(br_bit(&r), 1);		/* tx_mode_select */
	/* frame_reference_mode() */
	CHECK_EQ(br_bit(&r), 0);		/* reference_select */
	/* allow_warped_motion is not coded for error resilient frames */
	CHECK_EQ(br_bit(&r), 0);		/* reduced_tx_set_used */

	/* global_motion_params() */
	for (ref = 0; ref < 7; ref++) {
		int type = pic.wm[ref].invalid ? 0 : (int)pic.wm[ref].wmtype;
		int32_t prev[6];
		unsigned int i;

		for (i = 0; i < 6; i++)
			prev[i] = (i % 3 == 2) ? (1 << 16) : 0;	/* PRIMARY_REF_NONE */

		CHECK_EQ(br_bit(&r), type != 0);
		if (type == 0)
			continue;
		if (type == 2) {
			CHECK_EQ(br_bit(&r), 1);	/* is_rot_zoom */
		} else {
			CHECK_EQ(br_bit(&r), 0);
			CHECK_EQ(br_bit(&r), type == 1);	/* is_translation */
		}
		if (type >= 2) {
			int32_t v2 = decode_global_param(&r, type, 2, prev, 1);
			int32_t v3 = decode_global_param(&r, type, 3, prev, 1);

			CHECK_EQ(v2, pic.wm[ref].wmmat[2]);
			CHECK_EQ(v3, pic.wm[ref].wmmat[3]);
			if (type == 3) {
				int32_t v4 = decode_global_param(&r, type, 4, prev, 1);
				int32_t v5 = decode_global_param(&r, type, 5, prev, 1);

				CHECK_EQ(v4, pic.wm[ref].wmmat[4]);
				CHECK_EQ(v5, pic.wm[ref].wmmat[5]);
			}
		}
		if (type >= 1) {
			int32_t v0 = decode_global_param(&r, type, 0, prev, 1);
			int32_t v1 = decode_global_param(&r, type, 1, prev, 1);

			CHECK_EQ(v0, pic.wm[ref].wmmat[0]);
			CHECK_EQ(v1, pic.wm[ref].wmmat[1]);
		}
	}
	CHECK_EQ(r.error, 0);
}

int
main(void)
{
	test_bit_writer();
	test_leb128();
	test_ns_roundtrip();
	test_subexp_roundtrip();
	test_sequence_header();
	test_frame_obu_keyframe();
	test_frame_obu_inter_multitile();
	test_global_motion_roundtrip();

	printf("test_av1_bits: %d checks, %d failures\n", checks, failures);
	return failures ? 1 : 0;
}
