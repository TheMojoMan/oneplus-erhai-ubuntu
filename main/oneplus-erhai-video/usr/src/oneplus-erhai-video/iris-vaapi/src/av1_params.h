// SPDX-License-Identifier: GPL-2.0-or-later
/* AV1 header synthesis for the Iris stateful V4L2 decoder. */

#ifndef IRIS_VAAPI_AV1_PARAMS_H
#define IRIS_VAAPI_AV1_PARAMS_H

#include <stddef.h>
#include <stdint.h>
#include <va/va.h>
#include <va/va_dec_av1.h>

#define AV1_NUM_REF_FRAMES	8
#define AV1_REFS_PER_FRAME	7
#define AV1_MAX_TILES		64
#define AV1_TILE_SIZE_BYTES	4

/* frame_type values from the AV1 specification (VA carries them verbatim). */
enum av1_frame_type {
	AV1_FRAME_KEY		= 0,
	AV1_FRAME_INTER		= 1,
	AV1_FRAME_INTRA_ONLY	= 2,
	AV1_FRAME_SWITCH	= 3,
};

/* Everything the sequence header carries that the driver can derive from the
 * VA picture parameters.  A new sequence header is emitted whenever this
 * changes. */
struct av1_seq_cfg {
	unsigned int profile;
	unsigned int bit_depth;		/* 8, 10 or 12 */
	unsigned int mono_chrome;
	unsigned int color_range;
	unsigned int subsampling_x, subsampling_y;
	unsigned int film_grain_params_present;
	unsigned int still_picture;
	unsigned int use_128x128_superblock;
	unsigned int enable_filter_intra;
	unsigned int enable_intra_edge_filter;
	unsigned int enable_interintra_compound;
	unsigned int enable_masked_compound;
	unsigned int enable_warped_motion;
	unsigned int enable_ref_frame_mvs;
	unsigned int enable_dual_filter;
	unsigned int enable_order_hint;
	unsigned int enable_jnt_comp;
	unsigned int enable_cdef;
	unsigned int enable_superres;
	unsigned int enable_restoration;
	unsigned int separate_uv_delta_q;
	unsigned int order_hint_bits;
	unsigned int max_width, max_height;
};

/* One tile of the current frame: a byte range in the caller's tile buffer. */
struct av1_tile {
	uint32_t offset;
	uint32_t size;
	uint16_t row, column;
};

/* DPB state the driver has to mirror to synthesize valid headers: per-slot
 * order hints (motion vector prediction) and per-slot global motion parameters
 * (PrevGmParams recentering).  Which slots a frame refreshed is decided by the
 * caller and replayed through av1_writer_commit(). */
struct av1_writer {
	struct av1_seq_cfg seq;
	int seq_valid;
	unsigned int frame_index;	/* tracing only */
	uint8_t slot_order_hint[AV1_NUM_REF_FRAMES];
	uint8_t slot_valid[AV1_NUM_REF_FRAMES];
	int32_t slot_gm[AV1_NUM_REF_FRAMES][AV1_REFS_PER_FRAME][6];
};

void av1_writer_reset(struct av1_writer *w);

/* Derive the sequence configuration from a picture parameter buffer. */
void av1_seq_from_pic(const VADecPictureParameterBufferAV1 *pic,
		      unsigned int width, unsigned int height,
		      struct av1_seq_cfg *cfg);
int av1_seq_equal(const struct av1_seq_cfg *a, const struct av1_seq_cfg *b);

/* The sequence-header flags VA's picture parameters do not carry, but which
 * decide the *syntax* of a synthesized frame header: enable_warped_motion
 * gates the allow_warped_motion bit, enable_cdef gates the CDEF parameters,
 * enable_superres/restoration/separate_uv_delta_q gate their frame-level
 * syntax.  A frame header is only parseable against the sequence header that
 * the stream actually carries, so these have to match the encoder's.
 * enable_ref_frame_mvs is carried for completeness; it constrains rather than
 * gates syntax. */
struct av1_seq_tools {
	unsigned int superres;
	unsigned int cdef;
	unsigned int restoration;
	unsigned int separate_uv_delta_q;
	unsigned int warped_motion;
	unsigned int ref_frame_mvs;
};

/* Parse the encoder's own sequence header OBU payload for the flags in
 * struct av1_seq_tools.  Returns 1 and fills @tools on success, 0 when the
 * header uses a construct this walk does not handle (the caller then keeps
 * its defaults). */
int av1_parse_seq_tools(const uint8_t *payload, size_t len,
			struct av1_seq_tools *tools);

/* Write an OBU_SEQUENCE_HEADER (obu_has_size_field = 1).  Returns the byte
 * count, or 0 when @cap is too small. */
size_t av1_write_sequence_header(const struct av1_seq_cfg *cfg,
				 uint8_t *out, size_t cap);

/* Upper bound for the OBU_FRAME produced by av1_write_frame_obu(). */
size_t av1_frame_obu_bound(unsigned int tile_count,
			   const struct av1_tile *tiles);

/* Write one OBU_FRAME: synthesized frame header plus the tile group.  The
 * writer's DPB state is updated for @refresh_frame_flags.  @ref_idx overrides
 * the seven reference slots (so the header can point at the driver's own DPB
 * instead of the client's); NULL keeps the encoder's mapping.  Returns the byte
 * count, or 0 on failure. */
size_t av1_write_frame_obu(struct av1_writer *w,
			   const VADecPictureParameterBufferAV1 *pic,
			   const struct av1_tile *tiles,
			   unsigned int tile_count,
			   const uint8_t *tile_data,
			   unsigned int refresh_frame_flags,
			   const unsigned int *ref_idx,
			   int tile_size_prefix,
			   uint8_t *out, size_t cap);

#endif /* IRIS_VAAPI_AV1_PARAMS_H */
