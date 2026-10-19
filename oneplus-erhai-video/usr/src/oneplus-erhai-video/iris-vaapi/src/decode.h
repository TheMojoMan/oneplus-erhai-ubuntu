// SPDX-License-Identifier: GPL-2.0-or-later
/* Decode backend for the iris-vaapi driver: wraps the V4L2 engine, tracks VA
 * surfaces, accumulates picture parameters + slices, reassembles a whole H.264
 * access unit and maps decoded NV12 CAPTURE buffers back to VA surfaces.
 *
 * Threading/lifecycle model (matches how Chrome uses libva):
 * - Surfaces live in a display-level registry (iris_surfs) and may outlive
 *   the decode context that produced them.
 * - Each VA context owns its own iris_decode_ctx (its own V4L2 session), so
 *   concurrent videos do not share engine state and switching codec or
 *   resolution always starts from a clean firmware session.
 */

#ifndef IRIS_VAAPI_DECODE_H
#define IRIS_VAAPI_DECODE_H

#include <stdint.h>
#include <stddef.h>
#include <va/va.h>

struct iris_decode_ctx;
struct iris_surfs;

/* ---- Display-level surface registry ---- */
struct iris_surfs *iris_surfs_create(void);
void iris_surfs_destroy(struct iris_surfs *t);
int iris_surfs_alloc(struct iris_surfs *t, VASurfaceID id,
		     unsigned int width, unsigned int height,
		     unsigned int fourcc);
void iris_surfs_free(struct iris_surfs *t, VASurfaceID id);
/* Sync routing: drains the owning engine until @id is ready.  Never-queued,
 * already-decoded surfaces and surfaces whose engine is gone succeed
 * immediately (Chrome syncs pool surfaces before decoding into them). */
int iris_surfs_sync(struct iris_surfs *t, VASurfaceID id);
/* Advisory readiness without draining an engine. */
int iris_surfs_ready(struct iris_surfs *t, VASurfaceID id);
int iris_surfs_valid(struct iris_surfs *t, VASurfaceID id);
/* Export the backing of @id as a fresh DRM PRIME fd (caller owns it). */
int iris_surfs_export(struct iris_surfs *t, VASurfaceID id, int *fd,
		      unsigned int *pitch, unsigned int *size,
		      unsigned int *width, unsigned int *height,
		      unsigned int *fourcc);
/* Backing memory layout of @id (for vaDeriveImage/vaGetImage). */
int iris_surfs_buffer(struct iris_surfs *t, VASurfaceID id, void **mem,
		      unsigned int *pitch, unsigned int *size,
		      unsigned int *width, unsigned int *height,
		      unsigned int *fourcc);

/* ---- Per-context decode engines ---- */
struct iris_decode_ctx *iris_decode_create(void);
void iris_decode_destroy(struct iris_decode_ctx *ctx);

void iris_decode_setup(struct iris_decode_ctx *ctx, unsigned int width,
		       unsigned int height, VAProfile profile);

/* Attach the display-level registry this engine reads/writes surfaces in. */
void iris_decode_set_surfaces(struct iris_decode_ctx *ctx,
			      struct iris_surfs *t);
/* Bind context render targets for the opt-in direct CAPTURE DMA-BUF ring. */
int iris_decode_set_render_targets(struct iris_decode_ctx *ctx,
				   const VASurfaceID *targets,
				   unsigned int count);
/* Legacy shims: allocate/free a surface in the attached registry. */
int iris_decode_create_surface(struct iris_decode_ctx *ctx, VASurfaceID id);
void iris_decode_destroy_surface(struct iris_decode_ctx *ctx, VASurfaceID id);

/* Update coded dimensions of a context that has not started decoding. */
void iris_decode_reconfigure(struct iris_decode_ctx *ctx, unsigned int width,
			     unsigned int height);
/* Tear down the firmware session and per-stream state (seek/reset). */
void iris_decode_reset(struct iris_decode_ctx *ctx);

/* Wait until @id has a decoded frame (drains this engine). */
int iris_decode_sync(struct iris_decode_ctx *ctx, VASurfaceID id);
/* Feed EOS so the firmware releases the picture it is holding. */
int iris_decode_flush(struct iris_decode_ctx *ctx);

/* Per-picture accumulation (called from vaRenderPicture). */
int iris_decode_begin(struct iris_decode_ctx *ctx, VASurfaceID target);
int iris_decode_picture(struct iris_decode_ctx *ctx,
			const VAPictureParameterBufferH264 *pic);
int iris_decode_h264_iq_matrix(struct iris_decode_ctx *ctx,
			       const VAIQMatrixBufferH264 *iq);
/* Capture per-slice parameters (reference counts) for the PPS default. */
int iris_decode_slice_params(struct iris_decode_ctx *ctx,
			     const VASliceParameterBufferH264 *sp);
/* HEVC: picture/slice params used to canonicalize parameter sets and RPS. */
int iris_decode_hevc_picture(struct iris_decode_ctx *ctx,
			     const VAPictureParameterBufferHEVC *pic);
int iris_decode_hevc_iq_matrix(struct iris_decode_ctx *ctx,
			       const VAIQMatrixBufferHEVC *iq);
int iris_decode_hevc_slice_params(struct iris_decode_ctx *ctx,
				  const VASliceParameterBufferHEVC *sp);
int iris_decode_slice(struct iris_decode_ctx *ctx, const void *data,
		      size_t len);

/* Reassemble and feed the engine.  Returns once queued (async). */
int iris_decode_end(struct iris_decode_ctx *ctx);

#endif
