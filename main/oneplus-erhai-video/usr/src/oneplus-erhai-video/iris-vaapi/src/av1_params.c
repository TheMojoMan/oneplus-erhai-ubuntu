// SPDX-License-Identifier: GPL-2.0-or-later
/* AV1 header synthesis for the Iris stateful V4L2 decoder.
 *
 * VA-API delivers AV1 as fully parsed picture parameters plus one CABAC-coded
 * payload per tile; the firmware wants a complete OBU access unit.  This module
 * rebuilds the sequence header and the uncompressed frame header from the
 * VA picture parameter buffer and wraps the untouched tile payloads in a
 * tile group.  Bit-exactness of the reconstructed headers matters: the tile
 * payload is entropy coded *against* them, so every syntax element that
 * influences CABAC context selection has to be reproduced faithfully.
 *
 * The AV1 specification is written as a parser, so each writer function below
 * mirrors its syntax paragraph (5.5.1, 5.5.2, 5.9.2, 5.9.5, 5.9.8, 5.9.11,
 * 5.9.12, 5.9.14, 5.9.15, 5.9.17, 5.9.18, 5.9.19, 5.9.20, 5.9.21, 5.9.24,
 * 5.9.30) field by field, in specification order.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "av1_bits.h"
#include "av1_params.h"

/* VA-API documents no value order for the loop-restoration types, and the
 * two client classes that reach this driver disagree:
 * - libavcodec built against the av1fix libva passes the raw AV1
 *   syntax order (0 none, 1 switchable, 2 wiener, 3 sgrproj);
 * - stock-libva clients (Chromium, Brave) pass the DXVA/Vulkan enum
 *   order (0 none, 1 wiener, 2 sgrproj, 3 switchable).
 * The av1fix tail field (refresh_frame_flags_valid, which the refresh
 * parse already uses to tell the classes apart) selects the mapping:
 * writing the wrong one desyncs the tile data's LR coefficient section
 * and the frame fails to decode at all. */
static const uint8_t va_lr_syntax_to_av1[4] = { 0, 1, 2, 3 };
static const uint8_t va_lr_dxva_to_av1[4] = { 0, 2, 3, 1 };

static const uint8_t *
va_lr_map_for(const VADecPictureParameterBufferAV1 *pic)
{
	return pic->refresh_frame_flags_valid ?
		va_lr_syntax_to_av1 : va_lr_dxva_to_av1;
}

/* Specification constants. */
#define AV1_MAX_TILE_WIDTH	4096
#define AV1_MAX_TILE_AREA	(4096 * 2304)
#define AV1_MAX_TILE_COLS	64
#define AV1_MAX_TILE_ROWS	64
#define AV1_SUPERRES_NUM	8
#define AV1_SUPERRES_DENOM_MIN	9
#define AV1_SUPERRES_DENOM_BITS	3
#define AV1_WARPEDMODEL_PREC_BITS 16

#define AV1_GM_ABS_TRANS_BITS		12
#define AV1_GM_ABS_TRANS_ONLY_BITS	9
#define AV1_GM_ABS_ALPHA_BITS		12
#define AV1_GM_ALPHA_PREC_BITS		15
#define AV1_GM_TRANS_PREC_BITS		6
#define AV1_GM_TRANS_ONLY_PREC_BITS	3

#define AV1_PRIMARY_REF_NONE	7

enum av1_warped_model {
	AV1_IDENTITY		= 0,
	AV1_TRANSLATION		= 1,
	AV1_ROTZOOM		= 2,
	AV1_AFFINE		= 3,
};

/* SEG_LVL_* feature coding tables (spec 5.9.14). */
static const uint8_t seg_feature_bits[8] = { 8, 6, 6, 6, 6, 3, 0, 0 };
static const uint8_t seg_feature_signed[8] = { 1, 1, 1, 1, 1, 0, 0, 0 };

enum {
	SEG_LVL_ALT_Q = 0,
	SEG_LVL_MAX = 8,
};

#define AV1_HEADER_SCRATCH 1024

/* Section tracing for bitstream debugging (IRIS_AV1_MARK=1). */
static int mark_enabled(void)
{
	static int on = -1;

	if (on < 0)
		on = getenv("IRIS_AV1_MARK") != NULL;
	return on;
}

#define MARK(bw, name) do {						\
	if (mark_enabled())						\
		fprintf(stderr, "[av1-bits] %-24s %zu bits\n", name,	\
			(bw)->pos * 8 + (bw)->bit);			\
} while (0)

static void
gm_identity(int32_t gm[6])
{
	unsigned int i;

	for (i = 0; i < 6; i++)
		gm[i] = (i % 3 == 2) ? (1 << AV1_WARPEDMODEL_PREC_BITS) : 0;
}

static int32_t
clip3(int32_t lo, int32_t hi, int32_t v)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

void
av1_writer_reset(struct av1_writer *w)
{
	unsigned int i, r;

	memset(w, 0, sizeof(*w));
	for (i = 0; i < AV1_NUM_REF_FRAMES; i++)
		for (r = 0; r < AV1_REFS_PER_FRAME; r++)
			gm_identity(w->slot_gm[i][r]);
}

void
av1_seq_from_pic(const VADecPictureParameterBufferAV1 *pic,
		 unsigned int width, unsigned int height,
		 struct av1_seq_cfg *cfg)
{
	const uint32_t seq = pic->seq_info_fields.value;

	memset(cfg, 0, sizeof(*cfg));
	cfg->profile = pic->profile;
	cfg->bit_depth = pic->bit_depth_idx == 0 ? 8 :
			 pic->bit_depth_idx == 1 ? 10 : 12;
	cfg->mono_chrome = (seq >> 10) & 1;
	cfg->color_range = (seq >> 11) & 1;
	cfg->subsampling_x = (seq >> 12) & 1;
	cfg->subsampling_y = (seq >> 13) & 1;
	cfg->film_grain_params_present = (seq >> 15) & 1;
	cfg->still_picture = seq & 1;
	cfg->use_128x128_superblock = (seq >> 1) & 1;
	cfg->enable_filter_intra = (seq >> 2) & 1;
	cfg->enable_intra_edge_filter = (seq >> 3) & 1;
	cfg->enable_interintra_compound = (seq >> 4) & 1;
	cfg->enable_masked_compound = (seq >> 5) & 1;
	/* Not carried by VA; signalled as available, exactly as before. */
	cfg->enable_warped_motion = 1;
	cfg->enable_ref_frame_mvs = 1;
	cfg->enable_dual_filter = (seq >> 6) & 1;
	cfg->enable_order_hint = (seq >> 7) & 1;
	cfg->enable_jnt_comp = (seq >> 8) & 1;
	cfg->enable_cdef = (seq >> 9) & 1;
	/* enable_superres, enable_restoration and separate_uv_delta_q are not
	 * part of VA's sequence information.  Signal all three as available,
	 * always: deriving them from the current picture makes the sequence
	 * header change mid-stream when a later picture starts using loop
	 * restoration, and re-emitting the sequence header resets the reference
	 * buffers (dav1d then rejects every picture until the next key frame).
	 * Enabling a tool only adds trailing syntax; a picture that does not use
	 * it codes use_superres = 0 / RESTORE_NONE, so decoding is unchanged. */
	cfg->enable_superres = 1;
	cfg->enable_restoration = 1;
	cfg->separate_uv_delta_q = 1;
	cfg->order_hint_bits = pic->order_hint_bits_minus_1 + 1;
	cfg->max_width = width;
	cfg->max_height = height;

	/* Only Main profile 8-bit 4:2:0 is wired up (the kernel advertises Main
	 * only); keep the derived subsampling coherent for the other profiles
	 * so an accidental Profile 1/2 stream cannot produce a broken header. */
	if (cfg->profile == 0) {
		cfg->subsampling_x = 1;
		cfg->subsampling_y = 1;
	} else if (cfg->profile == 1) {
		cfg->mono_chrome = 0;
		cfg->subsampling_x = 0;
		cfg->subsampling_y = 0;
	}
	if (cfg->mono_chrome)
		cfg->subsampling_x = cfg->subsampling_y = 1;
	if (!cfg->enable_order_hint) {
		cfg->order_hint_bits = 0;
		cfg->enable_jnt_comp = 0;
	}
}

int
av1_seq_equal(const struct av1_seq_cfg *a, const struct av1_seq_cfg *b)
{
	return memcmp(a, b, sizeof(*a)) == 0;
}

/* spec 5.5.1 sequence_header_obu(), read far enough to recover the four
 * flags VA does not carry.  Mirrors the emission order of
 * write_sequence_header plus the sections the encoder may code and the
 * writer forces off (timing info, decoder model, initial display delay,
 * the color description). */
int
av1_parse_seq_tools(const uint8_t *payload, size_t len,
		    struct av1_seq_tools *tools)
{
	size_t pos = 0;
	unsigned int profile, reduced, v, i;

	#define RB(n) do {							\
		v = 0;							\
		for (unsigned _k = 0; _k < (n); _k++) {			\
			if ((pos >> 3) >= len)				\
				return 0;				\
			v = (v << 1) |					\
				((payload[pos >> 3] >> (7 - (pos & 7))) & 1); \
			pos++;						\
		}							\
	} while (0)

	if (len < 2)
		return 0;
	RB(3);					/* seq_profile */
	profile = v;
	RB(1);					/* still_picture */
	reduced = v;
	{
		unsigned int order_hint, wbits, hbits;
		unsigned int mono = 0, cdp = 0, cp = 0, tc = 0, mc = 0;
		unsigned int twelve = 0, high_bitdepth = 0;

		if (!reduced) {
			unsigned int timing, model = 0, display, opcnt;

			RB(1);			/* timing_info_present_flag */
			timing = v;
			if (timing) {
				RB(32);		/* num_units_in_display_tick */
				RB(32);		/* time_scale */
				RB(1);		/* equal_picture_interval */
				if (v)
					RB(32);	/* num_ticks_per_picture_minus_1 */
			}
			RB(1);			/* decoder_model_info_present_flag */
			model = v;
			if (model) {
				RB(5);		/* buffer_delay_length_minus_1 */
				RB(32);		/* num_units_in_decoding_tick */
				RB(5);		/* buffer_removal_time_length_minus_1 */
				RB(5);		/* frame_presentation_time_length_minus_1 */
			}
			RB(1);			/* initial_display_delay_present_flag */
			display = v;
			RB(5);			/* operating_points_cnt_minus_1 */
			opcnt = v;
			for (i = 0; i <= opcnt; i++) {
				unsigned int level;

				if (i > 31)
					return 0;
				RB(12);		/* operating_point_idc[i] */
				RB(5);		/* seq_level_idx[i] */
				level = v;
				if (level > 7)
					RB(1);	/* seq_tier[i] */
				if (model) {
					RB(1);	/* operating_parameters_present_flag */
					if (v)
						return 0; /* delay bits: unhandled */
				}
				if (display)
					RB(5);	/* initial_display_delay_minus_1[i] */
			}
		} else {
			RB(5);			/* seq_level_idx[0] */
		}

		RB(4);				/* frame_width_bits_minus_1 */
		wbits = v + 1;
		RB(4);				/* frame_height_bits_minus_1 */
		hbits = v + 1;
		RB(wbits);			/* max_frame_width_minus_1 */
		RB(hbits);			/* max_frame_height_minus_1 */
		if (!reduced) {
			RB(1);			/* frame_id_numbers_present_flag */
			if (v) {
				RB(4);		/* delta_frame_id_length_minus_2 */
				RB(3);		/* additional_frame_id_length_minus_1 */
			}
		}
		RB(1);				/* use_128x128_superblock */
		RB(1);				/* enable_filter_intra */
		RB(1);				/* enable_intra_edge_filter */
		if (reduced) {
			/* Tools forced off without being coded. */
			*tools = (struct av1_seq_tools){ 0 };
			return 1;
		}
		RB(1);				/* enable_interintra_compound */
		RB(1);				/* enable_masked_compound */
		RB(1);				/* enable_warped_motion */
		tools->warped_motion = v;
		RB(1);				/* enable_dual_filter */
		RB(1);				/* enable_order_hint */
		order_hint = v;
		if (order_hint) {
			RB(1);			/* enable_jnt_comp */
			RB(1);			/* enable_ref_frame_mvs */
			tools->ref_frame_mvs = v;
		}
		RB(1);				/* seq_choose_screen_content_tools */
		if (!v)
			RB(1);			/* seq_force_screen_content_tools */
		else
			v = 2;			/* SELECT_SCREEN_CONTENT_TOOLS */
		if (v > 0) {
			RB(1);			/* seq_choose_integer_mv */
			if (!v)
				RB(1);		/* seq_force_integer_mv */
		}
		if (order_hint)
			RB(3);			/* order_hint_bits_minus_1 */
		RB(1);				/* enable_superres */
		tools->superres = v;
		RB(1);				/* enable_cdef */
		tools->cdef = v;
		RB(1);				/* enable_restoration */
		tools->restoration = v;

		/* color_config(), spec 5.5.2.  The order matters: subsampling is
		 * only coded when the colour description is not the identity
		 * case, and color_range comes first.  Getting this wrong shifts
		 * separate_uv_delta_q by a few bits - harmless while the driver
		 * wrote its own sequence header, fatal when a synthesized frame
		 * header has to be parseable against the encoder's. */
		RB(1);				/* high_bitdepth */
		high_bitdepth = v;
		if (profile == 2 && high_bitdepth) {
			RB(1);			/* twelve_bit */
			twelve = v;
		}
		if (profile != 1) {
			RB(1);			/* mono_chrome */
			mono = v;
		}
		RB(1);				/* color_description_present_flag */
		cdp = v;
		if (cdp) {
			RB(8);			/* color_primaries */
			cp = v;
			RB(8);			/* transfer_characteristics */
			tc = v;
			RB(8);			/* matrix_coefficients */
			mc = v;
		}
		if (mono) {
			RB(1);			/* color_range */
		} else if (cdp && cp == 1 && tc == 13 && mc == 0) {
			/* CP_BT_709 + TC_SRGB + MC_IDENTITY: implied range/subsampling */
		} else {
			unsigned int ssx = 1, ssy = 1;

			RB(1);			/* color_range */
			if (profile == 0) {
				ssx = ssy = 1;
			} else if (profile == 1) {
				ssx = ssy = 0;
			} else if (twelve) {
				RB(1);		/* subsampling_x */
				ssx = v;
				if (ssx) {
					RB(1);	/* subsampling_y */
					ssy = v;
				} else {
					ssy = 0;
				}
			} else {
				ssx = 1;
				ssy = 0;
			}
			if (ssx && ssy)
				RB(2);		/* chroma_sample_position */
		}
		RB(1);				/* separate_uv_delta_q */
		tools->separate_uv_delta_q = v;
	}
	#undef RB
	return 1;
}

static unsigned int
seq_level_idx(unsigned int width, unsigned int height)
{
	uint64_t pixels = (uint64_t)width * height;

	if (pixels <= (uint64_t)1920 * 1088)
		return 8;	/* 4.0 */
	if (pixels <= (uint64_t)3840 * 2176)
		return 13;	/* 5.1 */
	return 17;		/* 6.1 */
}

/* spec 5.5.1 sequence_header_obu() */
static void
write_sequence_header(struct av1_bw *bw, const struct av1_seq_cfg *cfg)
{
	unsigned int wbits, hbits, level;
	unsigned int separate_uv_delta_q;

	/* still_picture sequences are not produced by VA clients; forcing 0
	 * keeps reduced_still_picture_header legal. */
	av1_bw_bits(bw, cfg->profile, 3);
	av1_bw_bit(bw, 0);		/* still_picture */
	av1_bw_bit(bw, 0);		/* reduced_still_picture_header */

	av1_bw_bit(bw, 0);		/* timing_info_present_flag */
	av1_bw_bit(bw, 0);		/* initial_display_delay_present_flag */
	av1_bw_bits(bw, 0, 5);		/* operating_points_cnt_minus_1 */

	av1_bw_bits(bw, 0, 12);		/* operating_point_idc[0] */
	level = seq_level_idx(cfg->max_width, cfg->max_height);
	av1_bw_bits(bw, level, 5);	/* seq_level_idx[0] */
	if (level > 7)
		av1_bw_bit(bw, 0);	/* seq_tier[0]: Main */

	wbits = av1_bit_width(cfg->max_width - 1);
	hbits = av1_bit_width(cfg->max_height - 1);
	av1_bw_bits(bw, wbits - 1, 4);	/* frame_width_bits_minus_1 */
	av1_bw_bits(bw, hbits - 1, 4);	/* frame_height_bits_minus_1 */
	av1_bw_bits(bw, cfg->max_width - 1, wbits);
	av1_bw_bits(bw, cfg->max_height - 1, hbits);

	av1_bw_bit(bw, 0);		/* frame_id_numbers_present_flag */

	av1_bw_bit(bw, cfg->use_128x128_superblock);
	av1_bw_bit(bw, cfg->enable_filter_intra);
	av1_bw_bit(bw, cfg->enable_intra_edge_filter);

	av1_bw_bit(bw, cfg->enable_interintra_compound);
	av1_bw_bit(bw, cfg->enable_masked_compound);
	/* This gates allow_warped_motion in every frame header, so it must
	 * match the sequence header in force, not just look permissive. */
	av1_bw_bit(bw, cfg->enable_warped_motion);
	av1_bw_bit(bw, cfg->enable_dual_filter);
	av1_bw_bit(bw, cfg->enable_order_hint);
	if (cfg->enable_order_hint) {
		av1_bw_bit(bw, cfg->enable_jnt_comp);
		av1_bw_bit(bw, cfg->enable_ref_frame_mvs);
	}

	/* seq_force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS and
	 * seq_force_integer_mv = SELECT_INTEGER_MV: the frame header then
	 * always codes both flags, so the VA values are reproduced exactly. */
	av1_bw_bit(bw, 1);		/* seq_choose_screen_content_tools */
	av1_bw_bit(bw, 1);		/* seq_choose_integer_mv */

	if (cfg->enable_order_hint)
		av1_bw_bits(bw, cfg->order_hint_bits - 1, 3);

	/* enable_superres / enable_restoration are not carried by VA either.
	 * Signalling them as available is safe: both only add trailing syntax,
	 * and a stream that uses neither ends up with use_superres = 0 and
	 * RESTORE_NONE, i.e. identical decoding. */
	av1_bw_bit(bw, cfg->enable_superres);
	av1_bw_bit(bw, cfg->enable_cdef);
	av1_bw_bit(bw, cfg->enable_restoration);

	/* color_config() */
	av1_bw_bit(bw, cfg->bit_depth > 8);
	if (cfg->profile == 2 && cfg->bit_depth > 8)
		av1_bw_bit(bw, cfg->bit_depth == 12);
	if (cfg->profile != 1)
		av1_bw_bit(bw, cfg->mono_chrome);
	av1_bw_bit(bw, 0);		/* color_description_present_flag */
	separate_uv_delta_q = cfg->mono_chrome ? 0 : cfg->separate_uv_delta_q;
	if (cfg->mono_chrome) {
		av1_bw_bit(bw, cfg->color_range);
	} else {
		av1_bw_bit(bw, cfg->color_range);
		/* subsampling is fixed by the profile (4:2:0 for Main) */
		if (cfg->subsampling_x && cfg->subsampling_y)
			av1_bw_bits(bw, 0, 2);	/* chroma_sample_position */
	}
	av1_bw_bit(bw, separate_uv_delta_q);

	av1_bw_bit(bw, cfg->film_grain_params_present);
}

size_t
av1_sequence_header_size(const struct av1_seq_cfg *cfg)
{
	uint8_t scratch[64];
	struct av1_bw bw;

	av1_bw_init(&bw, scratch, sizeof(scratch));
	write_sequence_header(&bw, cfg);
	av1_bw_trailing_bits(&bw);
	if (bw.error)
		return 0;
	return 1 + 2 + av1_bw_size(&bw);
}

size_t
av1_write_sequence_header(const struct av1_seq_cfg *cfg,
			  uint8_t *out, size_t cap)
{
	uint8_t payload[64];
	struct av1_bw bw;
	size_t payload_len, hdr_len;

	av1_bw_init(&bw, payload, sizeof(payload));
	write_sequence_header(&bw, cfg);
	av1_bw_trailing_bits(&bw);
	if (bw.error)
		return 0;
	payload_len = av1_bw_size(&bw);

	hdr_len = 1 + av1_leb128_size(payload_len);
	if (hdr_len + payload_len > cap)
		return 0;
	out[0] = (uint8_t)((1 << 3) | 0x02);	/* OBU_SEQUENCE_HEADER, size field */
	av1_leb128_write(out + 1, payload_len);
	memcpy(out + hdr_len, payload, payload_len);
	return hdr_len + payload_len;
}

/* spec 5.9.15 tile_info() */
struct tile_geometry {
	unsigned int sb_cols, sb_rows, sb_shift;
	unsigned int tile_cols_log2, tile_rows_log2;
	unsigned int tile_cols, tile_rows;
};

static unsigned int
tile_log2(unsigned int blk_size, unsigned int target)
{
	unsigned int k = 0;

	while ((blk_size << k) < target)
		k++;
	return k;
}

static unsigned int
uniform_tile_count(unsigned int sb_units, unsigned int log2)
{
	unsigned int width_sb = (sb_units + (1u << log2) - 1) >> log2;
	unsigned int start, count = 0;

	for (start = 0; start < sb_units; start += width_sb)
		count++;
	return count;
}

static void
write_tile_info(struct av1_bw *bw, const VADecPictureParameterBufferAV1 *pic,
		const struct av1_seq_cfg *cfg, unsigned int frame_width,
		unsigned int frame_height, struct tile_geometry *geo)
{
	unsigned int mi_cols = 2 * ((frame_width + 7) >> 3);
	unsigned int mi_rows = 2 * ((frame_height + 7) >> 3);
	unsigned int sb_cols, sb_rows, sb_shift, sb_size;
	unsigned int max_tile_width_sb, max_tile_area_sb;
	unsigned int min_log2_tile_cols, max_log2_tile_cols, max_log2_tile_rows;
	unsigned int min_log2_tiles;
	unsigned int want_cols, want_rows;
	unsigned int l2;

	if (cfg->use_128x128_superblock) {
		sb_cols = (mi_cols + 31) >> 5;
		sb_rows = (mi_rows + 31) >> 5;
		sb_shift = 5;
	} else {
		sb_cols = (mi_cols + 15) >> 4;
		sb_rows = (mi_rows + 15) >> 4;
		sb_shift = 4;
	}
	if (!sb_cols)
		sb_cols = 1;
	if (!sb_rows)
		sb_rows = 1;
	sb_size = sb_shift + 2;
	max_tile_width_sb = AV1_MAX_TILE_WIDTH >> sb_size;
	max_tile_area_sb = AV1_MAX_TILE_AREA >> (2 * sb_size);

	min_log2_tile_cols = tile_log2(max_tile_width_sb, sb_cols);
	max_log2_tile_cols = tile_log2(1, sb_cols < AV1_MAX_TILE_COLS ?
				      sb_cols : AV1_MAX_TILE_COLS);
	max_log2_tile_rows = tile_log2(1, sb_rows < AV1_MAX_TILE_ROWS ?
				      sb_rows : AV1_MAX_TILE_ROWS);
	min_log2_tiles = min_log2_tile_cols;
	{
		unsigned int area = tile_log2(max_tile_area_sb, sb_rows * sb_cols);

		if (area > min_log2_tiles)
			min_log2_tiles = area;
	}

	want_cols = pic->tile_cols ? pic->tile_cols : 1;
	want_rows = pic->tile_rows ? pic->tile_rows : 1;

	geo->sb_cols = sb_cols;
	geo->sb_rows = sb_rows;
	geo->sb_shift = sb_shift;

	if (pic->pic_info_fields.bits.uniform_tile_spacing_flag) {
		unsigned int chosen_cols = min_log2_tile_cols;
		unsigned int chosen_rows;

		for (l2 = min_log2_tile_cols; l2 <= max_log2_tile_cols; l2++) {
			if (uniform_tile_count(sb_cols, l2) == want_cols) {
				chosen_cols = l2;
				break;
			}
		}
		av1_bw_bit(bw, 1);	/* uniform_tile_spacing_flag */
		for (l2 = min_log2_tile_cols; l2 < chosen_cols; l2++)
			av1_bw_bit(bw, 1);	/* increment_tile_cols_log2 */
		if (chosen_cols < max_log2_tile_cols)
			av1_bw_bit(bw, 0);

		{
			unsigned int min_rows = min_log2_tiles > chosen_cols ?
				min_log2_tiles - chosen_cols : 0;

			chosen_rows = min_rows;
			for (l2 = min_rows; l2 <= max_log2_tile_rows; l2++) {
				if (uniform_tile_count(sb_rows, l2) ==
				    want_rows) {
					chosen_rows = l2;
					break;
				}
			}
			for (l2 = min_rows; l2 < chosen_rows; l2++)
				av1_bw_bit(bw, 1);	/* increment_tile_rows_log2 */
			if (chosen_rows < max_log2_tile_rows)
				av1_bw_bit(bw, 0);

			geo->tile_cols_log2 = chosen_cols;
			geo->tile_rows_log2 = chosen_rows;
			geo->tile_cols = uniform_tile_count(sb_cols,
							    chosen_cols);
			geo->tile_rows = uniform_tile_count(sb_rows,
							    chosen_rows);
		}
	} else {
		unsigned int start, i, widest = 0;

		av1_bw_bit(bw, 0);
		for (i = 0, start = 0; start < sb_cols; i++) {
			unsigned int avail = sb_cols - start;
			unsigned int max_width = avail < max_tile_width_sb ?
				avail : max_tile_width_sb;
			unsigned int value = i < AV1_MAX_TILES ?
				pic->width_in_sbs_minus_1[i] : 0;
			unsigned int size_sb;

			value = value > max_width - 1 ? max_width - 1 : value;
			av1_bw_ns(bw, value, max_width);
			size_sb = value + 1;
			if (size_sb > widest)
				widest = size_sb;
			start += size_sb;
		}
		geo->tile_cols = i;
		geo->tile_cols_log2 = tile_log2(1, i);

		if (min_log2_tiles > 0)
			max_tile_area_sb = (sb_rows * sb_cols) >>
				(min_log2_tiles + 1);
		else
			max_tile_area_sb = sb_rows * sb_cols;
		{
			unsigned int max_tile_height_sb = max_tile_area_sb /
				(widest ? widest : 1);

			if (!max_tile_height_sb)
				max_tile_height_sb = 1;
			for (i = 0, start = 0; start < sb_rows; i++) {
				unsigned int avail = sb_rows - start;
				unsigned int max_height =
					avail < max_tile_height_sb ? avail :
					max_tile_height_sb;
				unsigned int value = i < AV1_MAX_TILES ?
					pic->height_in_sbs_minus_1[i] : 0;

				value = value > max_height - 1 ? max_height - 1 :
					value;
				av1_bw_ns(bw, value, max_height);
				start += value + 1;
			}
			geo->tile_rows = i;
			geo->tile_rows_log2 = tile_log2(1, i);
		}
	}

	if (geo->tile_cols_log2 > 0 || geo->tile_rows_log2 > 0) {
		unsigned int bits = geo->tile_cols_log2 + geo->tile_rows_log2;
		unsigned int id = pic->context_update_tile_id;

		av1_bw_bits(bw, id & ((1u << bits) - 1), bits);
		av1_bw_bits(bw, AV1_TILE_SIZE_BYTES - 1, 2);
	}
}

static void
write_delta_q(struct av1_bw *bw, int32_t delta)
{
	if (delta) {
		av1_bw_bit(bw, 1);
		av1_bw_su(bw, delta, 7);
	} else {
		av1_bw_bit(bw, 0);
	}
}

static unsigned int g_gm_frame;

/* What the decoder reconstructs from subexp code word @v. */
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
gm_reconstruct(unsigned int v, int32_t r_arg, unsigned int num_syms,
	       int32_t mx, unsigned int prec_diff, int32_t round)
{
	int32_t u;

	/* decode_unsigned_subexp_with_ref(), then (value << precDiff) + round */
	if ((int64_t)r_arg * 2 <= (int64_t)num_syms)
		u = inverse_recenter(r_arg, (int32_t)v);
	else
		u = (int32_t)num_syms - 1 -
		    inverse_recenter((int32_t)(num_syms - 1) - r_arg,
				     (int32_t)v);
	return (u - mx) * (int32_t)(1u << prec_diff) + round;
}

/* Invert read_global_param(): find the subexp code word that decodes to
 * @target given the recentering reference value @prev. */
static void
write_global_param(struct av1_bw *bw, int type, int idx, int32_t target,
		   const int32_t prev[6], int allow_high_precision_mv)
{
	unsigned int abs_bits = AV1_GM_ABS_ALPHA_BITS;
	unsigned int prec_bits = AV1_GM_ALPHA_PREC_BITS;
	unsigned int prec_diff, mx, num_syms, r_arg;
	int32_t sub, round, x, u, v, r;
	unsigned int i, k;
	uint32_t mk;

	if (idx < 2) {
		if (type == AV1_TRANSLATION) {
			abs_bits = AV1_GM_ABS_TRANS_ONLY_BITS -
				!allow_high_precision_mv;
			prec_bits = AV1_GM_TRANS_ONLY_PREC_BITS -
				!allow_high_precision_mv;
		} else {
			abs_bits = AV1_GM_ABS_TRANS_BITS;
			prec_bits = AV1_GM_TRANS_PREC_BITS;
		}
	}
	prec_diff = AV1_WARPEDMODEL_PREC_BITS - prec_bits;
	round = (idx % 3) == 2 ? (1 << AV1_WARPEDMODEL_PREC_BITS) : 0;
	sub = (idx % 3) == 2 ? (1 << (int)prec_bits) : 0;
	mx = 1u << abs_bits;

	r = (prev[idx] >> prec_diff) - sub;
	num_syms = 2 * mx + 1;
	r_arg = (unsigned int)(r + (int32_t)mx);

	x = (target - round) >> prec_diff;	/* arithmetic shift */
	u = x + (int32_t)mx;

	if ((int32_t)r_arg < 0) {
		v = u;
	} else {
		int32_t rl = (int32_t)r_arg;

		if (r_arg << 1 <= num_syms) {
			v = (u < rl) ? 2 * (rl - u) - 1 :
			    (u <= 2 * rl) ? 2 * (u - rl) : u;
		} else {
			int32_t rb = (int32_t)(num_syms - 1 - r_arg);
			int32_t ub = (int32_t)(num_syms - 1 - u);

			v = (ub < rb) ? 2 * (rb - ub) - 1 :
			    (ub <= 2 * rb) ? 2 * (ub - rb) : ub;
		}
	}
	if (v < 0)
		v = 0;
	if ((uint32_t)v >= num_syms)
		v = (int32_t)(num_syms - 1);
	if (getenv("IRIS_AV1_GM_DEBUG"))
		fprintf(stderr,
			"[gm] f=%u type=%d idx=%d target=%d prev=%d v=%d recon=%d\n",
			g_gm_frame, type, idx, target, prev[idx], v,
			gm_reconstruct((unsigned int)v, (int32_t)r_arg, num_syms,
				       (int32_t)mx, prec_diff, round));
	if (gm_reconstruct((unsigned int)v, (int32_t)r_arg, num_syms,
			   (int32_t)mx, prec_diff, round) != target) {
		uint32_t best = 0, k;
		int64_t best_err = -1;

		for (k = 0; k < num_syms; k++) {
			int32_t got = gm_reconstruct(k, (int32_t)r_arg, num_syms,
						     (int32_t)mx, prec_diff,
						     round);
			int64_t err = (int64_t)got - target;

			if (err < 0)
				err = -err;
			if (best_err < 0 || err < best_err) {
				best_err = err;
				best = k;
				if (!err)
					break;
			}
		}
		v = (int32_t)best;
		if (getenv("IRIS_AV1_GM_DEBUG"))
			fprintf(stderr,
				"[gm] f=%u type=%d idx=%d INEXACT target=%d best_err=%lld v=%d\n",
				g_gm_frame, type, idx, target,
				(long long)best_err, v);
	}

	/* Code the value with decode_subexp()'s word structure. */
	i = 0;
	k = 3;
	mk = 0;
	for (;;) {
		unsigned int b2 = i ? k + i - 1 : k;
		uint32_t a = 1u << b2;

		if (num_syms <= mk + 3 * a) {
			uint32_t value = (uint32_t)v;

			if (value < mk)
				value = mk;
			if (value >= num_syms)
				value = num_syms - 1;
			av1_bw_ns(bw, value - mk, num_syms - mk);
			return;
		}
		if ((uint32_t)v < mk + a) {
			av1_bw_bit(bw, 0);
			av1_bw_bits(bw, (uint32_t)v - mk, b2);
			return;
		}
		av1_bw_bit(bw, 1);
		i++;
		mk += a;
	}
}

static void
write_global_motion(struct av1_bw *bw, const VADecPictureParameterBufferAV1 *pic,
		    const int32_t prev_gm[AV1_REFS_PER_FRAME][6],
		    int allow_high_precision_mv)
{
	unsigned int ref;

	for (ref = 0; ref < AV1_REFS_PER_FRAME; ref++) {
		const VAWarpedMotionParamsAV1 *wm = &pic->wm[ref];
		int type = wm->invalid ? AV1_IDENTITY : (int)wm->wmtype;

		if (type == AV1_IDENTITY) {
			av1_bw_bit(bw, 0);	/* is_global */
			continue;
		}
		av1_bw_bit(bw, 1);		/* is_global */
		if (type == AV1_ROTZOOM) {
			av1_bw_bit(bw, 1);	/* is_rot_zoom */
		} else {
			av1_bw_bit(bw, 0);	/* is_rot_zoom */
			av1_bw_bit(bw, type == AV1_TRANSLATION);
		}
		if (type >= AV1_ROTZOOM) {
			write_global_param(bw, type, 2, wm->wmmat[2],
					   prev_gm[ref],
					   allow_high_precision_mv);
			write_global_param(bw, type, 3, wm->wmmat[3],
					   prev_gm[ref],
					   allow_high_precision_mv);
			if (type == AV1_AFFINE) {
				write_global_param(bw, type, 4, wm->wmmat[4],
						   prev_gm[ref],
						   allow_high_precision_mv);
				write_global_param(bw, type, 5, wm->wmmat[5],
						   prev_gm[ref],
						   allow_high_precision_mv);
			}
		}
		if (type >= AV1_TRANSLATION) {
			write_global_param(bw, type, 0, wm->wmmat[0],
					   prev_gm[ref],
					   allow_high_precision_mv);
			write_global_param(bw, type, 1, wm->wmmat[1],
					   prev_gm[ref],
					   allow_high_precision_mv);
		}
	}
}

/* spec 5.9.30 film_grain_params() */
static void
write_film_grain(struct av1_bw *bw, const VADecPictureParameterBufferAV1 *pic,
		 const struct av1_seq_cfg *cfg, unsigned int frame_type,
		 unsigned int show_frame, unsigned int showable_frame)
{
	const VAFilmGrainStructAV1 *fg = &pic->film_grain_info;
	unsigned int apply = fg->film_grain_info_fields.bits.apply_grain;
	unsigned int chroma_from_luma;
	unsigned int num_pos_luma, num_pos_chroma;
	unsigned int i;

	if (!cfg->film_grain_params_present || (!show_frame && !showable_frame))
		return;
	av1_bw_bit(bw, apply);
	if (!apply)
		return;

	av1_bw_bits(bw, fg->grain_seed, 16);
	if (frame_type == AV1_FRAME_INTER)
		av1_bw_bit(bw, 1);	/* update_grain */

	av1_bw_bits(bw, fg->num_y_points, 4);
	for (i = 0; i < fg->num_y_points; i++) {
		av1_bw_bits(bw, fg->point_y_value[i], 8);
		av1_bw_bits(bw, fg->point_y_scaling[i], 8);
	}

	if (cfg->mono_chrome)
		chroma_from_luma = 0;
	else
		chroma_from_luma =
			fg->film_grain_info_fields.bits.chroma_scaling_from_luma;
	if (!cfg->mono_chrome)
		av1_bw_bit(bw, chroma_from_luma);

	{
		unsigned int force_zero = cfg->mono_chrome || chroma_from_luma ||
			(cfg->subsampling_x && cfg->subsampling_y &&
			 fg->num_y_points == 0);

		if (force_zero) {
			/* num_cb_points / num_cr_points are inferred to 0 */
		} else {
			av1_bw_bits(bw, fg->num_cb_points, 4);
			for (i = 0; i < fg->num_cb_points; i++) {
				av1_bw_bits(bw, fg->point_cb_value[i], 8);
				av1_bw_bits(bw, fg->point_cb_scaling[i], 8);
			}
			av1_bw_bits(bw, fg->num_cr_points, 4);
			for (i = 0; i < fg->num_cr_points; i++) {
				av1_bw_bits(bw, fg->point_cr_value[i], 8);
				av1_bw_bits(bw, fg->point_cr_scaling[i], 8);
			}
		}
	}

	av1_bw_bits(bw, fg->film_grain_info_fields.bits.grain_scaling_minus_8, 2);
	av1_bw_bits(bw, fg->film_grain_info_fields.bits.ar_coeff_lag, 2);
	num_pos_luma = 2 * fg->film_grain_info_fields.bits.ar_coeff_lag *
		(fg->film_grain_info_fields.bits.ar_coeff_lag + 1);
	num_pos_chroma = fg->num_y_points ? num_pos_luma + 1 : num_pos_luma;

	if (fg->num_y_points)
		for (i = 0; i < num_pos_luma; i++)
			av1_bw_bits(bw, (uint8_t)(fg->ar_coeffs_y[i] + 128), 8);
	if (chroma_from_luma || fg->num_cb_points)
		for (i = 0; i < num_pos_chroma; i++)
			av1_bw_bits(bw, (uint8_t)(fg->ar_coeffs_cb[i] + 128), 8);
	if (chroma_from_luma || fg->num_cr_points)
		for (i = 0; i < num_pos_chroma; i++)
			av1_bw_bits(bw, (uint8_t)(fg->ar_coeffs_cr[i] + 128), 8);

	av1_bw_bits(bw, fg->film_grain_info_fields.bits.ar_coeff_shift_minus_6, 2);
	av1_bw_bits(bw, fg->film_grain_info_fields.bits.grain_scale_shift, 2);
	if (fg->num_cb_points) {
		av1_bw_bits(bw, fg->cb_mult, 8);
		av1_bw_bits(bw, fg->cb_luma_mult, 8);
		av1_bw_bits(bw, fg->cb_offset, 9);
	}
	if (fg->num_cr_points) {
		av1_bw_bits(bw, fg->cr_mult, 8);
		av1_bw_bits(bw, fg->cr_luma_mult, 8);
		av1_bw_bits(bw, fg->cr_offset, 9);
	}
	av1_bw_bit(bw, fg->film_grain_info_fields.bits.overlap_flag);
	av1_bw_bit(bw, fg->film_grain_info_fields.bits.clip_to_restricted_range);
}

static int
get_relative_dist(unsigned int enable_order_hint, unsigned int order_hint_bits,
		  int a, int b)
{
	int diff, m;

	if (!enable_order_hint || !order_hint_bits)
		return 0;
	diff = a - b;
	m = 1 << (order_hint_bits - 1);
	return (diff & (m - 1)) - (diff & m);
}

/* spec 5.9.2 uncompressed_header() */
static void
write_uncompressed_header(struct av1_bw *bw, struct av1_writer *w,
			  const VADecPictureParameterBufferAV1 *pic,
			  const struct av1_seq_cfg *cfg,
			  unsigned int refresh_frame_flags,
			  const unsigned int *ref_idx,
			  struct tile_geometry *geo)
{
	unsigned int frame_type = pic->pic_info_fields.bits.frame_type;
	/* The Iris firmware only produces a picture for frames it believes are
	 * displayed: a hidden frame (show_frame = 0) comes back as an empty
	 * V4L2_BUF_FLAG_ERROR capture.  Always signal a shown picture; which
	 * picture actually reaches the screen is the client's decision, and the
	 * tile payload is unaffected because show_frame carries no coding state
	 * besides the reference bookkeeping that follows refresh_frame_flags. */
	unsigned int show_frame = 1;
	unsigned int showable_frame, frame_is_intra, er, allow_sct;
	unsigned int force_integer_mv, allow_intrabc = 0;
	unsigned int primary_ref_frame;
	unsigned int refresh;
	unsigned int num_planes = cfg->mono_chrome ? 1 : 3;
	unsigned int use_superres, superres_denom, upscaled_width, frame_width;
	unsigned int reference_select;
	unsigned int coded_lossless = 1, all_lossless;
	unsigned int tx_mode;
	int32_t prev_gm[AV1_REFS_PER_FRAME][6];
	unsigned int i, j;
	unsigned int oh_bits = cfg->enable_order_hint ? cfg->order_hint_bits : 0;

	if (frame_type == AV1_FRAME_SWITCH)
		frame_type = AV1_FRAME_INTER;
	/* A hidden key frame must not be reported as a shown key frame: that
	 * would make the decoder reset RefValid/RefOrderHint, which the encoder
	 * did not do (it would have had to re-signal the order hints).  Report
	 * it as INTRA_ONLY instead, which keeps the refresh set coded. */
	if (frame_type == AV1_FRAME_KEY &&
	    !pic->pic_info_fields.bits.show_frame)
		frame_type = AV1_FRAME_INTRA_ONLY;

	frame_is_intra = (frame_type == AV1_FRAME_KEY ||
			  frame_type == AV1_FRAME_INTRA_ONLY);
	showable_frame = show_frame ? (frame_type != AV1_FRAME_KEY) : 0;

	av1_bw_bit(bw, 0);			/* show_existing_frame */
	av1_bw_bits(bw, frame_type, 2);
	av1_bw_bit(bw, show_frame);
	if (!show_frame)
		av1_bw_bit(bw, showable_frame);

	if (frame_type == AV1_FRAME_KEY && show_frame) {
		er = 1;
	} else {
		er = pic->pic_info_fields.bits.error_resilient_mode;
		av1_bw_bit(bw, er);
	}

	av1_bw_bit(bw, pic->pic_info_fields.bits.disable_cdf_update);

	/* seq_force_screen_content_tools == SELECT: always coded. */
	allow_sct = pic->pic_info_fields.bits.allow_screen_content_tools;
	av1_bw_bit(bw, allow_sct);
	if (allow_sct)
		av1_bw_bit(bw, pic->pic_info_fields.bits.force_integer_mv);

	/* frame_id_numbers_present_flag == 0, frame_size_override_flag == 0 */
	av1_bw_bit(bw, 0);

	if (cfg->enable_order_hint)
		av1_bw_bits(bw, pic->order_hint & ((1u << oh_bits) - 1),
			    oh_bits);

	if (frame_is_intra || er) {
		primary_ref_frame = AV1_PRIMARY_REF_NONE;
	} else {
		primary_ref_frame = pic->primary_ref_frame & 7;
		av1_bw_bits(bw, primary_ref_frame, 3);
	}

	if (mark_enabled() && frame_type == AV1_FRAME_KEY && show_frame)
		fprintf(stderr, "[av1-refbits] au=%u off=-1\n",
			w->frame_index);
	if (frame_type == AV1_FRAME_KEY && show_frame) {
		refresh = 0xff;
	} else {
		refresh = refresh_frame_flags & 0xff;
		if (mark_enabled())
			fprintf(stderr, "[av1-refbits] au=%u off=%zu\n",
				w->frame_index, bw->pos * 8 + bw->bit);
		av1_bw_bits(bw, refresh, 8);
	}

	if ((!frame_is_intra || refresh != 0xff) && er && cfg->enable_order_hint) {
		/* Reproduce the decoder's current per-slot order hints so no
		 * reference is invalidated: VA carries the DPB map, not the
		 * coded ref_order_hint values. */
		for (i = 0; i < AV1_NUM_REF_FRAMES; i++)
			av1_bw_bits(bw, w->slot_order_hint[i] &
				    ((1u << oh_bits) - 1), oh_bits);
	}

	force_integer_mv = frame_is_intra ? 1 :
		(allow_sct && pic->pic_info_fields.bits.force_integer_mv);

	upscaled_width = pic->frame_width_minus1 + 1;
	if (upscaled_width > cfg->max_width)
		upscaled_width = cfg->max_width;
	frame_width = cfg->max_width;

	use_superres = !!(pic->pic_info_fields.bits.use_superres);
	if (use_superres) {
		unsigned int denom = pic->superres_scale_denominator;

		denom = clip3(AV1_SUPERRES_DENOM_MIN,
			      AV1_SUPERRES_DENOM_MIN + 7, denom);
		superres_denom = denom;
	} else {
		superres_denom = AV1_SUPERRES_NUM;
	}
	frame_width = (upscaled_width * AV1_SUPERRES_NUM +
		       superres_denom / 2) / superres_denom;

	/* frame_size(): empty with frame_size_override_flag == 0, but
	 * superres_params() still codes its bits at the start of it. */
#define WRITE_SUPERRES() do {						\
		if (!cfg->enable_superres)				\
			break;						\
		if (use_superres) {					\
			av1_bw_bit(bw, 1);				\
			av1_bw_bits(bw, superres_denom - AV1_SUPERRES_DENOM_MIN, \
				    AV1_SUPERRES_DENOM_BITS);		\
		} else {						\
			av1_bw_bit(bw, 0);				\
		}							\
	} while (0)

	if (frame_is_intra) {
		WRITE_SUPERRES();
		av1_bw_bit(bw, 0);	/* render_and_frame_size_different */
		if (allow_sct && upscaled_width == frame_width)
			av1_bw_bit(bw, allow_intrabc =
				   pic->pic_info_fields.bits.allow_intrabc);
	} else {
		if (cfg->enable_order_hint)
			av1_bw_bit(bw, 0);	/* frame_refs_short_signaling */
		for (i = 0; i < AV1_REFS_PER_FRAME; i++)
			av1_bw_bits(bw, (ref_idx ? ref_idx[i] :
				     pic->ref_frame_idx[i]) & 7, 3);
		WRITE_SUPERRES();
		av1_bw_bit(bw, 0);	/* render_and_frame_size_different */

		if (!force_integer_mv)
			av1_bw_bit(bw,
				   pic->pic_info_fields.bits.allow_high_precision_mv);

		/* read_interpolation_filter() */
		{
			unsigned int switchable = pic->interp_filter == 4;

			av1_bw_bit(bw, switchable);
			if (!switchable)
				av1_bw_bits(bw, pic->interp_filter & 3, 2);
		}

		av1_bw_bit(bw, pic->pic_info_fields.bits.is_motion_mode_switchable);
		if (!er && cfg->enable_order_hint)
			av1_bw_bit(bw,
				   pic->pic_info_fields.bits.use_ref_frame_mvs);
	}
#undef WRITE_SUPERRES

	if (pic->pic_info_fields.bits.disable_cdf_update)
		;	/* disable_frame_end_update_cdf inferred to 1 */
	else
		av1_bw_bit(bw,
			   pic->pic_info_fields.bits.disable_frame_end_update_cdf);

	MARK(bw, "pre-tile-info");
	write_tile_info(bw, pic, cfg, frame_width, cfg->max_height, geo);
	MARK(bw, "tile-info");

	/* quantization_params() */
	av1_bw_bits(bw, pic->base_qindex, 8);
	write_delta_q(bw, pic->y_dc_delta_q);
	{
		int32_t u_dc = num_planes > 1 ? pic->u_dc_delta_q : 0;
		int32_t u_ac = num_planes > 1 ? pic->u_ac_delta_q : 0;
		int32_t v_dc = num_planes > 1 ? pic->v_dc_delta_q : 0;
		int32_t v_ac = num_planes > 1 ? pic->v_ac_delta_q : 0;

		if (num_planes > 1) {
			unsigned int diff_uv = 0;

			if (cfg->separate_uv_delta_q) {
				diff_uv = (u_dc != v_dc) || (u_ac != v_ac);
				av1_bw_bit(bw, diff_uv);
			}
			write_delta_q(bw, u_dc);
			write_delta_q(bw, u_ac);
			if (diff_uv) {
				write_delta_q(bw, v_dc);
				write_delta_q(bw, v_ac);
			}
		}

		av1_bw_bit(bw, pic->qmatrix_fields.bits.using_qmatrix);
		if (pic->qmatrix_fields.bits.using_qmatrix) {
			av1_bw_bits(bw, pic->qmatrix_fields.bits.qm_y, 4);
			av1_bw_bits(bw, pic->qmatrix_fields.bits.qm_u, 4);
			if (cfg->separate_uv_delta_q)
				av1_bw_bits(bw, pic->qmatrix_fields.bits.qm_v, 4);
		}

		/* CodedLossless (spec 5.11.5 get_qindex with ignoreDeltaQ = 1) */
		for (i = 0; i < 8; i++) {
			int32_t qindex = pic->base_qindex;
			int lossless;

			if (pic->seg_info.segment_info_fields.bits.enabled &&
			    (pic->seg_info.feature_mask[i] &
			     (1u << SEG_LVL_ALT_Q)))
				qindex += pic->seg_info.feature_data[i][SEG_LVL_ALT_Q];
			qindex = clip3(0, 255, qindex);
			lossless = qindex == 0 && pic->y_dc_delta_q == 0 &&
				u_dc == 0 && u_ac == 0 && v_dc == 0 && v_ac == 0;
			if (!lossless)
				coded_lossless = 0;
		}
	}

	MARK(bw, "quantization");
	/* segmentation_params() */
	av1_bw_bit(bw, pic->seg_info.segment_info_fields.bits.enabled);
	if (pic->seg_info.segment_info_fields.bits.enabled) {
		unsigned int update_data;

		if (primary_ref_frame == AV1_PRIMARY_REF_NONE) {
			/* update_map = 1, temporal_update = 0, update_data = 1 */
			update_data = 1;
		} else {
			unsigned int update_map =
				pic->seg_info.segment_info_fields.bits.update_map;

			av1_bw_bit(bw, update_map);
			if (update_map)
				av1_bw_bit(bw,
					   pic->seg_info.segment_info_fields.bits.temporal_update);
			update_data =
				pic->seg_info.segment_info_fields.bits.update_data;
			av1_bw_bit(bw, update_data);
		}
		if (update_data) {
			for (i = 0; i < 8; i++) {
				for (j = 0; j < SEG_LVL_MAX; j++) {
					unsigned int enabled =
						(pic->seg_info.feature_mask[i] >>
						 j) & 1;
					int32_t value =
						pic->seg_info.feature_data[i][j];

					av1_bw_bit(bw, enabled);
					if (!enabled)
						continue;
					if (seg_feature_signed[j])
						av1_bw_su(bw, value,
							  1 + seg_feature_bits[j]);
					else
						av1_bw_bits(bw, (uint32_t)value,
							    seg_feature_bits[j]);
				}
			}
		}
	}

	MARK(bw, "segmentation");
	/* delta_q_params() */
	{
		unsigned int delta_q_present =
			pic->base_qindex ? pic->mode_control_fields.bits.delta_q_present_flag : 0;

		if (pic->base_qindex)
			av1_bw_bit(bw, delta_q_present);
		if (delta_q_present)
			av1_bw_bits(bw,
				    pic->mode_control_fields.bits.log2_delta_q_res,
				    2);

		/* delta_lf_params() */
		if (delta_q_present) {
			unsigned int delta_lf_present = 0;

			if (!allow_intrabc) {
				delta_lf_present =
					pic->mode_control_fields.bits.delta_lf_present_flag;
				av1_bw_bit(bw, delta_lf_present);
			}
			if (delta_lf_present) {
				av1_bw_bits(bw,
					    pic->mode_control_fields.bits.log2_delta_lf_res,
					    2);
				av1_bw_bit(bw,
					   pic->mode_control_fields.bits.delta_lf_multi);
			}
		}
	}

	MARK(bw, "delta-q/lf");
	/* loop_filter_params() */
	if (coded_lossless || allow_intrabc) {
		/* everything inferred */
	} else {
		av1_bw_bits(bw, pic->filter_level[0] & 0x3f, 6);
		av1_bw_bits(bw, pic->filter_level[1] & 0x3f, 6);
		if (num_planes > 1 && (pic->filter_level[0] ||
				       pic->filter_level[1])) {
			av1_bw_bits(bw, pic->filter_level_u & 0x3f, 6);
			av1_bw_bits(bw, pic->filter_level_v & 0x3f, 6);
		}
		av1_bw_bits(bw, pic->loop_filter_info_fields.bits.sharpness_level,
			    3);
		av1_bw_bit(bw,
			   pic->loop_filter_info_fields.bits.mode_ref_delta_enabled);
		if (pic->loop_filter_info_fields.bits.mode_ref_delta_enabled) {
			unsigned int update =
				pic->loop_filter_info_fields.bits.mode_ref_delta_update;

			av1_bw_bit(bw, update);
			if (update) {
				for (i = 0; i < 8; i++) {
					if (pic->ref_deltas[i]) {
						av1_bw_bit(bw, 1);
						av1_bw_su(bw,
							  pic->ref_deltas[i], 7);
					} else {
						av1_bw_bit(bw, 0);
					}
				}
				for (i = 0; i < 2; i++) {
					if (pic->mode_deltas[i]) {
						av1_bw_bit(bw, 1);
						av1_bw_su(bw,
							  pic->mode_deltas[i], 7);
					} else {
						av1_bw_bit(bw, 0);
					}
				}
			}
		}
	}

	MARK(bw, "loop-filter");
	/* cdef_params() */
	if (coded_lossless || allow_intrabc || !cfg->enable_cdef) {
		/* everything inferred */
	} else {
		unsigned int cdef_bits = pic->cdef_bits & 3;
		unsigned int count = 1u << cdef_bits;

		av1_bw_bits(bw, pic->cdef_damping_minus_3 & 3, 2);
		av1_bw_bits(bw, cdef_bits, 2);
		for (i = 0; i < count; i++) {
			unsigned int y = pic->cdef_y_strengths[i];

			av1_bw_bits(bw, (y >> 2) & 0xf, 4);
			av1_bw_bits(bw, y & 3, 2);
			if (num_planes > 1) {
				unsigned int uv = pic->cdef_uv_strengths[i];

				av1_bw_bits(bw, (uv >> 2) & 0xf, 4);
				av1_bw_bits(bw, uv & 3, 2);
			}
		}
	}

	MARK(bw, "cdef");
	/* lr_params() */
	all_lossless = coded_lossless && upscaled_width == frame_width;
	if (all_lossless || allow_intrabc || !cfg->enable_restoration) {
		/* enable_restoration is always signalled, but nothing is written
		 * when the frame is lossless or intrabc. */
	} else {
		unsigned int uses_lr = 0, uses_chroma_lr = 0;

		for (i = 0; i < num_planes; i++) {
			unsigned int type;

			if (i == 0)
				type = pic->loop_restoration_fields.bits.yframe_restoration_type;
			else if (i == 1)
				type = pic->loop_restoration_fields.bits.cbframe_restoration_type;
			else
				type = pic->loop_restoration_fields.bits.crframe_restoration_type;
			type = va_lr_map_for(pic)[type & 3];
			av1_bw_bits(bw, type, 2);
			if (type) {
				uses_lr = 1;
				if (i > 0)
					uses_chroma_lr = 1;
			}
		}
		if (uses_lr) {
			unsigned int shift =
				pic->loop_restoration_fields.bits.lr_unit_shift;

			if (cfg->use_128x128_superblock) {
				av1_bw_bit(bw, shift > 1);
			} else if (shift == 0) {
				av1_bw_bit(bw, 0);
			} else {
				av1_bw_bit(bw, 1);
				av1_bw_bit(bw, shift > 1);
			}
			if (cfg->subsampling_x && cfg->subsampling_y &&
			    uses_chroma_lr)
				av1_bw_bit(bw,
					   pic->loop_restoration_fields.bits.lr_uv_shift);
		}
	}

	MARK(bw, "lr");
	/* read_tx_mode() */
	tx_mode = pic->mode_control_fields.bits.tx_mode;
	if (!coded_lossless)
		av1_bw_bit(bw, tx_mode >= 2);

	MARK(bw, "tx-mode");
	/* frame_reference_mode() */
	reference_select = 0;
	if (frame_is_intra) {
		reference_select = 0;
	} else {
		reference_select =
			pic->mode_control_fields.bits.reference_select;
		av1_bw_bit(bw, reference_select);
	}

	MARK(bw, "ref-mode");
	/* skip_mode_params() */
	{
		unsigned int skip_allowed = 0;

		if (!frame_is_intra && reference_select &&
		    cfg->enable_order_hint) {
			int forward_idx = -1, backward_idx = -1;
			int forward_hint = 0, backward_hint = 0;
			int order_hint = pic->order_hint;

			for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
				int ref_hint = w->slot_order_hint[ref_idx ?
					(ref_idx[i] & 7) :
					(pic->ref_frame_idx[i] & 7)];
				int dist = get_relative_dist(cfg->enable_order_hint,
							     cfg->order_hint_bits,
							     ref_hint, order_hint);

				if (dist < 0) {
					if (forward_idx < 0 ||
					    get_relative_dist(cfg->enable_order_hint,
							      cfg->order_hint_bits,
							      ref_hint,
							      forward_hint) > 0) {
						forward_idx = (int)i;
						forward_hint = ref_hint;
					}
				} else if (dist > 0) {
					if (backward_idx < 0 ||
					    get_relative_dist(cfg->enable_order_hint,
							      cfg->order_hint_bits,
							      ref_hint,
							      backward_hint) < 0) {
						backward_idx = (int)i;
						backward_hint = ref_hint;
					}
				}
			}
			if (forward_idx >= 0 && backward_idx >= 0) {
				skip_allowed = 1;
			} else if (forward_idx >= 0) {
				int second = -1, second_hint = 0;

				for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
					int ref_hint = w->slot_order_hint[ref_idx ?
						(ref_idx[i] & 7) :
						(pic->ref_frame_idx[i] & 7)];

					if (get_relative_dist(cfg->enable_order_hint,
							      cfg->order_hint_bits,
							      ref_hint,
							      forward_hint) < 0) {
						if (second < 0 ||
						    get_relative_dist(cfg->enable_order_hint,
								      cfg->order_hint_bits,
								      ref_hint,
								      second_hint) > 0) {
							second = (int)i;
							second_hint = ref_hint;
						}
					}
				}
				if (second >= 0)
					skip_allowed = 1;
			}
		}
		if (skip_allowed)
			av1_bw_bit(bw,
				   pic->mode_control_fields.bits.skip_mode_present);
	}

	MARK(bw, "skip-mode");
	/* allow_warped_motion: enable_warped_motion is always 1 */
	if (!frame_is_intra && !er)
		av1_bw_bit(bw, pic->pic_info_fields.bits.allow_warped_motion);

	av1_bw_bit(bw, pic->mode_control_fields.bits.reduced_tx_set_used);

	MARK(bw, "warped-motion");
	/* global_motion_params(): read_global_param() predicts from
	 * PrevGmParams, which load_previous() sets to
	 * SavedGmParams[ref_frame_idx[primary_ref_frame]] - the gm_params of
	 * the picture sitting in the primary reference slot, saved when that
	 * picture was decoded.  With PRIMARY_REF_NONE setup_past_independence()
	 * leaves it identity. */
	{
		const unsigned int *idx = ref_idx ? ref_idx :
			(const unsigned int *)pic->ref_frame_idx;
		unsigned int prim = idx[primary_ref_frame < AV1_REFS_PER_FRAME ?
					primary_ref_frame : 0] & 7;

		for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
			if (primary_ref_frame == AV1_PRIMARY_REF_NONE)
				gm_identity(prev_gm[i]);
			else
				memcpy(prev_gm[i], w->slot_gm[prim][i],
				       sizeof(prev_gm[i]));
		}
	}
	if (!frame_is_intra)
		write_global_motion(bw, pic, (const int32_t(*)[6])prev_gm,
				    pic->pic_info_fields.bits.allow_high_precision_mv &&
				    !force_integer_mv);

	MARK(bw, "global-motion");
	write_film_grain(bw, pic, cfg, frame_type, show_frame, showable_frame);
	MARK(bw, "film-grain/end");
}

void
av1_writer_commit(struct av1_writer *w,
		  const VADecPictureParameterBufferAV1 *pic,
		  unsigned int refresh_frame_flags)
{
	unsigned int ref, i;
	int32_t decoded[AV1_REFS_PER_FRAME][6];
	/* global_motion_params() returns early for an intra frame, leaving every
	 * gm_params[] entry identity - so SavedGmParams[] must not pick up
	 * whatever the client left in wm[] for such a frame. */
	int intra_frame = pic->pic_info_fields.bits.frame_type == AV1_FRAME_KEY ||
			  pic->pic_info_fields.bits.frame_type == AV1_FRAME_INTRA_ONLY;

	for (ref = 0; ref < AV1_REFS_PER_FRAME; ref++) {
		const VAWarpedMotionParamsAV1 *wm = &pic->wm[ref];
		int type = intra_frame ? AV1_IDENTITY :
			   wm->invalid ? AV1_IDENTITY : (int)wm->wmtype;

		gm_identity(decoded[ref]);
		if (type == AV1_IDENTITY)
			continue;
		decoded[ref][0] = wm->wmmat[0];
		decoded[ref][1] = wm->wmmat[1];
		decoded[ref][2] = wm->wmmat[2];
		decoded[ref][3] = wm->wmmat[3];
		if (type == AV1_AFFINE) {
			decoded[ref][4] = wm->wmmat[4];
			decoded[ref][5] = wm->wmmat[5];
		} else if (type == AV1_ROTZOOM) {
			decoded[ref][4] = -decoded[ref][3];
			decoded[ref][5] = decoded[ref][2];
		}
	}

	for (i = 0; i < AV1_NUM_REF_FRAMES; i++) {
		if (!(refresh_frame_flags & (1u << i)))
			continue;
		w->slot_order_hint[i] = pic->order_hint;
		w->slot_valid[i] = 1;
		for (ref = 0; ref < AV1_REFS_PER_FRAME; ref++)
			memcpy(w->slot_gm[i][ref], decoded[ref],
			       sizeof(decoded[ref]));
	}
}

size_t
av1_frame_obu_bound(unsigned int tile_count, const struct av1_tile *tiles)
{
	size_t total = 1 + 8 + AV1_HEADER_SCRATCH + 2;
	unsigned int i;

	for (i = 0; i < tile_count; i++)
		total += tiles[i].size + AV1_TILE_SIZE_BYTES + 4;
	return total;
}

size_t
av1_write_frame_obu(struct av1_writer *w,
		    const VADecPictureParameterBufferAV1 *pic,
		    const struct av1_tile *tiles, unsigned int tile_count,
		    const uint8_t *tile_data, unsigned int refresh_frame_flags,
		    const unsigned int *ref_idx, int tile_size_prefix,
		    uint8_t *out, size_t cap)
{
	uint8_t header[AV1_HEADER_SCRATCH];
	struct tile_geometry geo;
	struct av1_bw bw;
	size_t header_len, tg_len, payload_len, hdr_len, total;
	unsigned int i;

	if (!tile_count)
		return 0;
	memset(&geo, 0, sizeof(geo));
	g_gm_frame = w->frame_index;
	av1_bw_init(&bw, header, sizeof(header));
	write_uncompressed_header(&bw, w, pic, &w->seq, refresh_frame_flags,
				  ref_idx, &geo);
	av1_bw_align(&bw);
	if (bw.error) {
		fprintf(stderr, "iris-vaapi: AV1 frame header overflow\n");
		return 0;
	}
	header_len = av1_bw_size(&bw);
	(void)geo;

	/* Tile group: tg_start/tg_end are only coded when the frame has more
	 * than one tile, and then only when the group does not cover the whole
	 * frame.  Everything VA clients hand over is a complete group. */
	tg_len = 0;
	if (tile_count > 1) {
		tg_len = 1;	/* tile_start_and_end_present_flag + alignment */
		if (tile_size_prefix)
			tg_len += (size_t)(tile_count - 1) * AV1_TILE_SIZE_BYTES;
	}
	for (i = 0; i < tile_count; i++)
		tg_len += tiles[i].size;

	payload_len = header_len + tg_len;
	hdr_len = 1 + av1_leb128_size(payload_len);
	total = hdr_len + payload_len;
	if (total > cap)
		return 0;

	out[0] = (uint8_t)((6 << 3) | 0x02);	/* OBU_FRAME, has size field */
	av1_leb128_write(out + 1, payload_len);
	memcpy(out + hdr_len, header, header_len);
	{
		uint8_t *p = out + hdr_len + header_len;

		if (tile_count > 1) {
			/* tile_start_and_end_present_flag = 0: the group covers
			 * the entire frame, then byte_alignment(). */
			*p++ = 0x00;
		}
		for (i = 0; i < tile_count; i++) {
			if (tile_size_prefix && i + 1 < tile_count) {
				/* tile_size_minus_1 is le(TileSizeBytes), i.e.
				 * little-endian, unlike every other field. */
				uint32_t size = tiles[i].size - 1;
				unsigned int b;

				for (b = 0; b < AV1_TILE_SIZE_BYTES; b++)
					p[b] = (uint8_t)((size >> (8 * b)) & 0xff);
				p += AV1_TILE_SIZE_BYTES;
			}
			if (tiles[i].size)
				memcpy(p, tile_data + tiles[i].offset,
				       tiles[i].size);
			p += tiles[i].size;
		}
	}

	av1_writer_commit(w, pic, refresh_frame_flags);
	w->frame_index++;
	return total;
}
