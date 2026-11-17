// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <pthread.h>

#include "decode.h"
#include "av1_params.h"
#include "h264_params.h"
#include "hevc_params.h"
#include "hevc_slice_rewrite.h"
#include "v4l2_dec.h"
#include "vk_copy.h"

/* Chrome runs one VaapiVideoDecoder per video; every decoder owns a frame
 * pool of up to ~32 surfaces and pools coexist across tabs/resolution
 * changes, so the registry must hold several pools at once. */
#define IRIS_MAX_SURFACES	128
#define IRIS_AU_MAX		(16U * 1024 * 1024)
#define IRIS_MAX_PENDING_COPIES	32
/* VA-API has no decoder-reset callback.  A random-access picture following a
 * user-visible pause is the best signal that a client reused its VAContext
 * across a seek.  Every codec drains and reopens its firmware session at this
 * boundary while preserving the monotonically increasing private timestamp
 * epoch across the restart. */
#define IRIS_SEEK_GAP_NS		100000000ULL

#ifndef ALIGN_TO
#define ALIGN_TO(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

static unsigned int
surface_pitch(unsigned int width, unsigned int fourcc)
{
	unsigned int bytes = fourcc == V4L2_PIX_FMT_P010 ? 2 : 1;
	unsigned int alignment = fourcc == V4L2_PIX_FMT_P010 ? 256 : 128;

	return ALIGN_TO(width * bytes, alignment);
}

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC	0x0001U
#endif

/* Per-frame tracing is extremely chatty and the GPU process inherits this
 * stderr; unconditionally writing it stalls the decode loop when the terminal
 * is slow.  Opt in with IRIS_VAAPI_DEBUG=1. */
static int g_dbg = -1;
static uint64_t g_buffer_serial;

static int dbg_enabled(void)
{
	if (g_dbg < 0)
		g_dbg = getenv("IRIS_VAAPI_DEBUG") != NULL;
	return g_dbg;
}

#define DBG(...)	do { if (dbg_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

static uint64_t
monotonic_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int
dma_heap_alloc(int heap_fd, unsigned int size)
{
	struct dma_heap_allocation_data data;

	memset(&data, 0, sizeof(data));
	data.len = size;
	data.fd = 0;
	data.fd_flags = O_RDWR | O_CLOEXEC;
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &data) < 0)
		return -1;
	return data.fd;
}

/* Fallback backing for when /dev/dma_heap/system is not accessible (root
 * only): an anonymous memfd is mmappable and readable by local tests and the
 * ffmpeg CPU readback path, but is NOT a DRM buffer and cannot be imported by
 * EGL/GPU clients like Chrome. */
static int
memfd_alloc(unsigned int size)
{
	int fd = (int)syscall(SYS_memfd_create, "iris-surface", MFD_CLOEXEC);

	if (fd < 0)
		return -1;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static int
dma_buf_cpu_sync(int fd, uint64_t flags)
{
	struct dma_buf_sync sync = { .flags = flags };
	int ret;

	do {
		ret = ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
	} while (ret < 0 && errno == EINTR);
	return ret;
}

static void
surface_fill_black(int fd, void *map, unsigned int pitch,
		   unsigned int height, unsigned int fourcc)
{
	size_t luma_size = (size_t)pitch * ALIGN_TO(height, 32);
	size_t total_size = luma_size * 3 / 2;
	int sync_started;

	sync_started = dma_buf_cpu_sync(fd,
		DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0;
	if (fourcc == V4L2_PIX_FMT_P010) {
		uint16_t *pixels = map;
		size_t i;

		/* P010 stores limited-range 10-bit components in the high bits. */
		for (i = 0; i < luma_size / sizeof(*pixels); i++)
			pixels[i] = 64U << 6;
		for (; i < total_size / sizeof(*pixels); i++)
			pixels[i] = 512U << 6;
	} else {
		/* Limited-range NV12 black: Y=16, neutral interleaved UV=128. */
		memset(map, 16, luma_size);
		memset((uint8_t *)map + luma_size, 128, total_size - luma_size);
	}
	if (sync_started)
		(void)dma_buf_cpu_sync(fd,
			DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
}

struct iris_surface {
	VASurfaceID id;
	int bfd;		/* backing fd (DMA-heap, or memfd fallback) */
	void *bmap;		/* mmap of the backing */
	unsigned int bsize;
	unsigned int sw, sh;	/* coded size the backing was created for */
	unsigned int fourcc;	/* VA/V4L2 layout: NV12 or P010 */
	int decoded;		/* a frame has been copied into the backing */
	int queued;		/* some picture was decoded into this surface */
	int exported;		/* backing has been exported to a DRM client */
	int write_started;	/* DMA_BUF_SYNC write access spans async decode */
	uint64_t fence_token;	/* pending kernel reservation fence, or zero */
	uint64_t generation;	/* render-target reuse generation */
	uint64_t backing_serial;	/* unique identity for Vulkan import cache */
	struct iris_decode_ctx *owner;	/* engine that queued the picture */
};

static int
surface_finish_write(struct v4l2_dec *dec, struct iris_surface *s)
{
	int ret = 0;

	if (s->write_started &&
	    dma_buf_cpu_sync(s->bfd,
			     DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE) < 0)
		ret = -errno;
	s->write_started = 0;
	if (s->fence_token) {
		int signal_ret = v4l2_dec_signal_surface_fence(dec,
							 s->fence_token);

		s->fence_token = 0;
		if (!ret && signal_ret)
			ret = signal_ret;
	}
	return ret;
}

static void
surface_begin_write(struct v4l2_dec *dec, struct iris_surface *s,
		    uint64_t token)
{
	if (s->write_started || s->fence_token)
		surface_finish_write(dec, s);
	if (dma_buf_cpu_sync(s->bfd,
			     DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) < 0)
		return;
	s->write_started = 1;
	if (!v4l2_dec_attach_surface_fence(dec, s->bfd, token)) {
		s->fence_token = token;
	} else {
		dma_buf_cpu_sync(s->bfd,
				 DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE);
		s->write_started = 0;
	}
}

static void
surface_begin_device_write(struct v4l2_dec *dec, struct iris_surface *s,
			   uint64_t token)
{
	if (s->write_started || s->fence_token)
		surface_finish_write(dec, s);
	if (!v4l2_dec_attach_surface_fence(dec, s->bfd, token))
		s->fence_token = token;
}

/* Display-level registry: surfaces may outlive the engine that decodes into
 * them (Chrome destroys contexts on navigation while pool surfaces drain). */
struct iris_surfs {
	struct iris_surface s[IRIS_MAX_SURFACES];
	int n;
	struct iris_vk_copy *vk_copy;
};

struct iris_decode_ctx {
	pthread_mutex_t mutex;
	struct v4l2_dec dec;
	int dec_open;
	int dec_started;
	unsigned int width, height;
	VAProfile profile;
	unsigned int out_pixfmt;	/* V4L2 OUTPUT pixel format */
	unsigned int cap_pixfmt;	/* V4L2 CAPTURE pixel format */
	int direct_capture;
	int direct_error;
	int fatal_error;
	struct iris_vk_copy *vk_copy;
	int vk_copy_failed;
	uint64_t vk_capture_generation;
	uint64_t vk_capture_keys[IRIS_MAX_SURFACES];
	struct {
		struct iris_vk_job *job;
		VASurfaceID id;
		uint64_t generation;
		uint64_t fence_token;
		uint64_t start_ns;
		size_t bytes;
		unsigned int capture_index;
		int used;
	} pending_copies[IRIS_MAX_PENDING_COPIES];
	unsigned int direct_count;
	unsigned int direct_requested_count;
	struct {
		VASurfaceID id;
		int fd;
		size_t size;
	} direct[IRIS_MAX_SURFACES];

	struct iris_surfs *surfs;	/* not owned */

	VAPictureParameterBufferH264 pic;
	int have_pic;
	VAIQMatrixBufferH264 h264_iq;
	int have_h264_iq;
	VAPictureParameterBufferHEVC hevc_pic;
	int have_hevc_pic;
	VAIQMatrixBufferHEVC hevc_iq;
	int have_hevc_iq;
	uint8_t *slice_data;
	size_t slice_len;
	size_t slice_cap;
	uint8_t *au_data;
	size_t au_cap;
	VASurfaceID current_target;
	uint64_t current_generation;

	uint8_t last_sps[256];
	int last_sps_len;
	uint8_t last_pps[4096];
	int last_pps_len;
	/* HEVC parameter-set cache (only emit when changed). */
	uint8_t last_hevc_vps[64];
	int last_hevc_vps_len;
	uint8_t last_hevc_sps[8192];
	int last_hevc_sps_len;
	uint8_t last_hevc_pps[128];
	int last_hevc_pps_len;
	/* Original parameter NALs, when the VA client supplies them in the
	 * slice-data buffer.  Stateful V4L2 needs these bytes verbatim. */
	uint8_t raw_hevc_vps[1024];
	uint8_t raw_hevc_sps[1024];
	uint8_t raw_hevc_pps[1024];
	int raw_hevc_vps_len;
	int raw_hevc_sps_len;
	int raw_hevc_pps_len;
	int refs_l0, refs_l1;	/* effective per-slice reference counts */
	int h264_pps_refs_l0, h264_pps_refs_l1;
	struct {
		VASliceParameterBufferH264 param;
	} h264_slices[128];
	unsigned int h264_slice_count;
	struct {
		VASliceParameterBufferHEVC param;
	} hevc_slices[128];
	unsigned int hevc_slice_count;
	unsigned int hevc_slice_next;
	int hevc_pps_id;
	int hevc_rewritten;

	/* AV1.  VA hands over parsed picture parameters plus one CABAC payload
	 * per tile, so the AU has to be synthesized (see av1_params.c).  The
	 * refresh set of a picture is only knowable from the *next* picture's
	 * DPB map, so decoding runs one frame behind: the assembled picture is
	 * held in "pending" and submitted when the following one arrives. */
	struct {
		int have_pic;
		VADecPictureParameterBufferAV1 pic;
		struct av1_slice {
			uint32_t offset;
			uint32_t size;
			uint16_t row, column;
		} slices[AV1_MAX_TILES];
		unsigned int n_slices;
		uint8_t *data;
		size_t data_len, data_cap;
		uint32_t blob_base;
		int blob_ready;

		int pending;
		VADecPictureParameterBufferAV1 pending_pic;
		uint8_t *pending_data;
		size_t pending_len, pending_cap;
		struct av1_slice pending_slices[AV1_MAX_TILES];
		uint64_t feed_ns;
		unsigned int pending_n_slices;
		VASurfaceID pending_target;
		uint64_t pending_generation;

		struct av1_writer writer;
		uint8_t *au;
		size_t au_cap;
		int dump_au;
		int dump_only;
		int no_tile_sizes;
		/* Our own DPB: which surface holds each slot, whether the current
		 * picture references it, and when it was last referenced. */
		VASurfaceID slot_surface[AV1_NUM_REF_FRAMES];
		uint8_t slot_used[AV1_NUM_REF_FRAMES];
		uint32_t slot_last_use[AV1_NUM_REF_FRAMES];
		uint32_t slot_stored_at[AV1_NUM_REF_FRAMES];
		/* How often each slot's picture has been referenced lately; a
		 * long-lived reference (the key frame, a golden frame) must not
		 * be the one we throw away. */
		unsigned int au_index;
		int high_bitdepth_warned;
		uint8_t last_order_hint;
		int have_order_hint;
		uint64_t substituted;
		uint64_t skips;
		/* Surface -> order hint of the pictures we decoded, so a lost
		 * reference can be replaced by the temporally closest one. */
		VASurfaceID hist_surface[64];
		uint8_t hist_order_hint[64];
		unsigned int hist_pos;
		unsigned int gop_pos;
	} av1;

	/* Map decode sequence numbers back to target surfaces so frame
	 * matching does not depend on the (possibly non-contiguous) VASurfaceID
	 * values that the client happens to use.  The ring is indexed by
	 * (seq & mask) and validated by the stored seq, so playback longer
	 * than any fixed table just wraps instead of breaking. */
#define IRIS_TARGET_RING	1024	/* power of two */
	uint64_t seq;
	struct {
		uint64_t seq;
		VASurfaceID target;
		uint64_t generation;
		int used;
	} target_ring[IRIS_TARGET_RING];
	VASurfaceID last_target;	/* most recently queued picture */
	int eos_sent;			/* EOS (v4l2_dec_flush) queued */
	/* HEVC firmware does not propagate usable per-frame timestamps.  Iris
	 * emits CAPTURE frames in display order, so retain POC-to-surface
	 * state as a strict FIFO ring: pictures complete in the order they
	 * were queued, and a ring can never overflow into a hard failure. */
#define IRIS_HEVC_RING	512
	struct {
		int32_t poc;
		VASurfaceID target;
		uint64_t generation;
	} hevc_ring[IRIS_HEVC_RING];
	unsigned int hevc_ring_head, hevc_ring_len;

	int stats_enabled;
	uint64_t stats_copy_ns;
	uint64_t stats_copy_bytes;
	uint64_t stats_copy_frames;
	uint64_t stats_vk_copy_ns;
	uint64_t stats_vk_copy_bytes;
	uint64_t stats_vk_copy_frames;
	uint64_t stats_vk_copy_fallbacks;
	uint64_t stats_capture_frames;
	uint64_t stats_sync_ns;
	uint64_t stats_rewrite_ns;
	uint64_t stats_rewrite_bytes;
	uint64_t stats_rewrites;
	uint64_t stats_end_ns;
	uint64_t stats_ends;
	uint64_t stats_h264_wait_ns;
	uint64_t stats_h264_waits;
	uint64_t stats_h264_async;
	uint64_t stats_direct_frames;
	uint64_t last_submit_ns;
	int vp9_seek_barrier;
	int force_h264_sync_end;
};

static struct iris_surface *
find_surface(struct iris_decode_ctx *ctx, VASurfaceID id);

static void av1_reset_session(struct iris_decode_ctx *ctx);
static void av1_reset_picture(struct iris_decode_ctx *ctx);

static int
surface_copy(struct iris_decode_ctx *ctx, struct iris_surface *s,
	     const void *src, size_t size)
{
	int sync_started = s->write_started;
	int ret = 0;
	uint64_t start;

	if (size > s->bsize)
		return -E2BIG;
	/* The backing is imported by Chrome's GPU process while this process
	 * updates it through an mmap.  DMA_BUF_IOCTL_SYNC supplies the required
	 * ownership/cache transition on non-coherent ARM systems; without it,
	 * 4K frames can be sampled with stale cache lines and appear torn or
	 * partially corrupted.  memfd fallback buffers do not support this
	 * ioctl and remain ordinary coherent CPU mappings. */
	if (!sync_started && dma_buf_cpu_sync(s->bfd,
				    DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE) == 0) {
		s->write_started = 1;
		sync_started = 1;
	} else if (!sync_started && errno != ENOTTY && errno != EINVAL) {
		return -errno;
	}

	start = ctx->stats_enabled ? monotonic_ns() : 0;
	memcpy(s->bmap, src, size);
	if (ctx->stats_enabled) {
		ctx->stats_copy_ns += monotonic_ns() - start;
		ctx->stats_copy_bytes += size;
		ctx->stats_copy_frames++;
	}

	if (sync_started) {
		start = ctx->stats_enabled ? monotonic_ns() : 0;
		ret = surface_finish_write(&ctx->dec, s);
		if (ctx->stats_enabled)
			ctx->stats_sync_ns += monotonic_ns() - start;
	}
	return ret;
}

static int
finish_vk_copy(struct iris_decode_ctx *ctx, unsigned int index, int wait)
{
	typeof(ctx->pending_copies[0]) *pending = &ctx->pending_copies[index];
	struct iris_surface *s;
	int ret, finish_ret = 0;

	if (!pending->used)
		return 0;
	ret = iris_vk_copy_job_wait(ctx->vk_copy, pending->job,
				    wait ? UINT64_MAX : 0);
	if (ret <= 0)
		return ret;

	s = find_surface(ctx, pending->id);
	if (s && s->generation == pending->generation &&
	    s->fence_token == pending->fence_token) {
		finish_ret = surface_finish_write(&ctx->dec, s);
		s->decoded = 1;
		s->queued = 1;
		s->owner = ctx;
	} else if (pending->fence_token) {
		/* The VA client should not destroy/recycle an in-flight target,
		 * but never leave its reservation fence permanently unsignalled if
		 * it does. */
		finish_ret = v4l2_dec_signal_surface_fence(
			&ctx->dec, pending->fence_token);
	}
	ret = v4l2_dec_qcap_idx(&ctx->dec, pending->capture_index);
	if (!finish_ret && ret)
		finish_ret = ret;
	if (ctx->stats_enabled) {
		ctx->stats_vk_copy_ns += monotonic_ns() - pending->start_ns;
		ctx->stats_vk_copy_bytes += pending->bytes;
		ctx->stats_vk_copy_frames++;
	}
	iris_vk_copy_job_release(pending->job);
	memset(pending, 0, sizeof(*pending));
	return finish_ret ? finish_ret : 1;
}

static int
reap_vk_copies(struct iris_decode_ctx *ctx, int wait_all)
{
	unsigned int i;
	int ret;

	if (!ctx->vk_copy)
		return 0;
	for (i = 0; i < ARRAY_SIZE(ctx->pending_copies); i++) {
		if (!ctx->pending_copies[i].used)
			continue;
		ret = finish_vk_copy(ctx, i, wait_all);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static void
forget_vk_capture_buffers(struct iris_decode_ctx *ctx)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctx->vk_capture_keys); i++) {
		if (ctx->vk_capture_keys[i])
			iris_vk_copy_forget(ctx->vk_copy,
					    ctx->vk_capture_keys[i]);
	}
	ctx->vk_capture_generation = 0;
	memset(ctx->vk_capture_keys, 0, sizeof(ctx->vk_capture_keys));
}

static int
finish_vk_surface(struct iris_decode_ctx *ctx, VASurfaceID id, int wait)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(ctx->pending_copies); i++) {
		int ret;

		if (!ctx->pending_copies[i].used ||
		    ctx->pending_copies[i].id != id)
			continue;
		ret = finish_vk_copy(ctx, i, wait);
		if (ret < 0)
			return ret;
		if (!ret)
			return -EAGAIN;
	}
	return 0;
}

static int
surface_vk_submit(struct iris_decode_ctx *ctx, struct iris_surface *s,
		  const struct v4l2_dec_frame *frame)
{
	unsigned int pitch, source_size;
	unsigned int slot;
	struct iris_vk_job *job;
	int source_fd, ret, completed = 0;

	if (!ctx->vk_copy || ctx->vk_copy_failed)
		return -ENOTSUP;
	if (frame->index >= ARRAY_SIZE(ctx->vk_capture_keys))
		return -ERANGE;
	if (ctx->vk_capture_generation != frame->capture_generation) {
		ret = reap_vk_copies(ctx, 1);
		if (ret)
			return ret;
		forget_vk_capture_buffers(ctx);
		ctx->vk_capture_generation = frame->capture_generation;
	}
	if (!ctx->vk_capture_keys[frame->index])
		ctx->vk_capture_keys[frame->index] = __atomic_add_fetch(
			&g_buffer_serial, 1, __ATOMIC_RELAXED);
	ret = v4l2_dec_export(&ctx->dec, frame->index, &source_fd, &pitch,
			      &source_size);
	if (ret)
		return ret;
	/* Chrome imports stable surfaces before decode.  Legacy ANGLE does not
	 * reliably observe reservation fences attached after that import, so
	 * finish the GPU copy before returning a dequeued exported surface.  The
	 * decode-only path below remains fully asynchronous. */
	if (s->exported) {
		uint64_t start = ctx->stats_enabled ? monotonic_ns() : 0;

		ret = iris_vk_copy_dmabuf(ctx->vk_copy,
					  ctx->vk_capture_keys[frame->index],
					  source_fd, source_size,
					  s->backing_serial, s->bfd, s->bsize,
					  frame->bytesused);
		if (!ret)
			ret = surface_finish_write(&ctx->dec, s);
		if (!ret) {
			if (ctx->stats_enabled) {
				ctx->stats_vk_copy_ns += monotonic_ns() - start;
				ctx->stats_vk_copy_bytes += frame->bytesused;
				ctx->stats_vk_copy_frames++;
			}
			completed = 1;
		}
		goto out;
	}
	for (slot = 0; slot < ARRAY_SIZE(ctx->pending_copies); slot++)
		if (!ctx->pending_copies[slot].used)
			break;
	if (slot == ARRAY_SIZE(ctx->pending_copies)) {
		ret = reap_vk_copies(ctx, 0);
		if (ret)
			goto out;
		for (slot = 0; slot < ARRAY_SIZE(ctx->pending_copies); slot++)
			if (!ctx->pending_copies[slot].used)
				break;
	}
	if (slot == ARRAY_SIZE(ctx->pending_copies)) {
		/* V4L2 currently has at most 20 CAPTURE buffers, so this is only
		 * a defensive pressure valve. */
		ret = finish_vk_copy(ctx, 0, 1);
		if (ret < 0)
			goto out;
		slot = 0;
	}

	ret = iris_vk_copy_submit(ctx->vk_copy,
				 ctx->vk_capture_keys[frame->index],
				 source_fd, source_size,
				 s->backing_serial, s->bfd, s->bsize,
				 frame->bytesused, &job);
	if (!ret) {
		ctx->pending_copies[slot].job = job;
		ctx->pending_copies[slot].id = s->id;
		ctx->pending_copies[slot].generation = s->generation;
		ctx->pending_copies[slot].fence_token = s->fence_token;
		ctx->pending_copies[slot].start_ns = monotonic_ns();
		ctx->pending_copies[slot].bytes = frame->bytesused;
		ctx->pending_copies[slot].capture_index = frame->index;
		ctx->pending_copies[slot].used = 1;
	}
out:
	close(source_fd);
	if (ret) {
		ctx->stats_vk_copy_fallbacks++;
		if (!ctx->vk_copy_failed)
			fprintf(stderr,
				"iris-vaapi: Vulkan DMA-BUF copy failed (%s); using CPU copy\n",
				strerror(-ret));
		ctx->vk_copy_failed = 1;
		return ret;
	}
	return completed ? 1 : 0;
}

static void
finish_pending_writes(struct iris_decode_ctx *ctx)
{
	int i;

	if (!ctx->surfs || !ctx->dec_open)
		return;
	(void)reap_vk_copies(ctx, 1);
	for (i = 0; i < ctx->surfs->n; i++) {
		struct iris_surface *s = &ctx->surfs->s[i];

		if (s->owner == ctx && (s->write_started || s->fence_token))
			surface_finish_write(&ctx->dec, s);
	}
}

static void
detach_owned_surfaces(struct iris_decode_ctx *ctx)
{
	int i;

	if (!ctx->surfs)
		return;
	for (i = 0; i < ctx->surfs->n; i++) {
		struct iris_surface *s = &ctx->surfs->s[i];

		if (s->owner == ctx)
			s->owner = NULL;
	}
}

static struct iris_surface *
surfs_find(struct iris_surfs *t, VASurfaceID id)
{
	int i;

	if (!t)
		return NULL;
	for (i = 0; i < t->n; i++)
		if (t->s[i].id == id)
			return &t->s[i];
	return NULL;
}

static struct iris_surface *
find_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	return surfs_find(ctx->surfs, id);
}

static void
target_ring_reset(struct iris_decode_ctx *ctx)
{
	memset(ctx->target_ring, 0, sizeof(ctx->target_ring));
}

/* Clear only state owned by one V4L2 firmware session.  Keep the picture that
 * may already have been collected between vaBeginPicture and vaEndPicture.
 *
 * This distinction matters after EOS: ensure_decoder() runs from
 * vaEndPicture, after the client has supplied the next picture.  Calling the
 * full reset_stream_state() there used to erase that picture's slice data and
 * parameters, so the first access unit after a seek/flush was incomplete. */
static void
reset_decoder_session_state(struct iris_decode_ctx *ctx)
{
	ctx->dec_started = 0;
	ctx->eos_sent = 0;
	ctx->last_target = 0;
	ctx->h264_pps_refs_l0 = -1;
	ctx->h264_pps_refs_l1 = -1;
	ctx->last_sps_len = 0;
	ctx->last_pps_len = 0;
	ctx->last_hevc_vps_len = 0;
	ctx->last_hevc_sps_len = 0;
	ctx->last_hevc_pps_len = 0;
	ctx->raw_hevc_vps_len = 0;
	ctx->raw_hevc_sps_len = 0;
	ctx->raw_hevc_pps_len = 0;
	ctx->hevc_ring_head = 0;
	ctx->hevc_ring_len = 0;
	ctx->direct_error = 0;
	ctx->fatal_error = 0;
	ctx->vk_capture_generation = 0;
	memset(ctx->vk_capture_keys, 0, sizeof(ctx->vk_capture_keys));
	target_ring_reset(ctx);
	ctx->seq = 0;
	ctx->last_submit_ns = 0;
	ctx->vp9_seek_barrier = 0;
	av1_reset_session(ctx);
}

/* Clear all per-stream decode state: parameter-set caches, the sequence to
 * surface mapping and EOS bookkeeping.  Surfaces and their backings are
 * preserved so clients may keep exporting them. */
static void
reset_stream_state(struct iris_decode_ctx *ctx)
{
	reset_decoder_session_state(ctx);
	ctx->have_pic = 0;
	ctx->have_h264_iq = 0;
	ctx->have_hevc_pic = 0;
	ctx->have_hevc_iq = 0;
	ctx->slice_len = 0;
	ctx->refs_l0 = 0;
	ctx->refs_l1 = 0;
	ctx->h264_slice_count = 0;
	ctx->hevc_slice_count = 0;
	ctx->hevc_slice_next = 0;
	ctx->hevc_pps_id = -1;
	ctx->hevc_rewritten = 0;
	av1_reset_picture(ctx);
}

struct iris_decode_ctx *
iris_decode_create(void)
{
	struct iris_decode_ctx *ctx = calloc(1, sizeof(*ctx));

	if (ctx) {
		pthread_mutexattr_t attr;

		pthread_mutexattr_init(&attr);
		pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
		pthread_mutex_init(&ctx->mutex, &attr);
		pthread_mutexattr_destroy(&attr);
		reset_stream_state(ctx);
		ctx->stats_enabled = getenv("IRIS_VAAPI_STATS") != NULL;
		ctx->force_h264_sync_end =
			getenv("IRIS_H264_SYNC_END") != NULL;
		ctx->av1.dump_au = getenv("IRIS_VAAPI_AV1_DUMP") != NULL;
		ctx->av1.dump_only = getenv("IRIS_AV1_DUMP_ONLY") != NULL;
		ctx->av1.no_tile_sizes = getenv("IRIS_AV1_TILE_SIZES") != NULL &&
			atoi(getenv("IRIS_AV1_TILE_SIZES")) == 0;
		av1_writer_reset(&ctx->av1.writer);
	}
	return ctx;
}

void
iris_decode_destroy(struct iris_decode_ctx *ctx)
{
	if (!ctx)
		return;
	if (ctx->dec_open) {
		/* Complete accepted work on normal context destruction.  Asynchronous
		 * clients such as FFmpeg's null sink may never call vaSyncSurface; a
		 * final EOS drain keeps teardown from silently abandoning in-flight
		 * pictures and makes end-to-end timing cover actual hardware work. */
		if (ctx->dec_started && !ctx->eos_sent && !ctx->fatal_error)
			(void)iris_decode_flush(ctx);
		finish_pending_writes(ctx);
		v4l2_dec_close(&ctx->dec);
	}
	if (ctx->stats_enabled) {
		double copy_sec = ctx->stats_copy_ns / 1e9;
		double vk_copy_sec = ctx->stats_vk_copy_ns / 1e9;

		fprintf(stderr,
			"[iris-stats] capture=%llu copied=%llu vk-copy=%llu fallback=%llu direct=%llu copy=%.3fs %.1fGiB/s vk-copy=%.3fs %.1fGiB/s sync=%.3fs rewrite=%llu/%.1fMiB %.3fs end=%llu %.3fs h264-wait=%llu/%.3fs async=%llu\n",
			(unsigned long long)ctx->stats_capture_frames,
			(unsigned long long)ctx->stats_copy_frames,
			(unsigned long long)ctx->stats_vk_copy_frames,
			(unsigned long long)ctx->stats_vk_copy_fallbacks,
			(unsigned long long)ctx->stats_direct_frames,
			copy_sec,
			copy_sec > 0 ? ctx->stats_copy_bytes / copy_sec /
				(1024.0 * 1024.0 * 1024.0) : 0.0,
			vk_copy_sec,
			vk_copy_sec > 0 ? ctx->stats_vk_copy_bytes / vk_copy_sec /
				(1024.0 * 1024.0 * 1024.0) : 0.0,
			ctx->stats_sync_ns / 1e9,
			(unsigned long long)ctx->stats_rewrites,
			ctx->stats_rewrite_bytes / (1024.0 * 1024.0),
			ctx->stats_rewrite_ns / 1e9,
			(unsigned long long)ctx->stats_ends,
			ctx->stats_end_ns / 1e9,
			(unsigned long long)ctx->stats_h264_waits,
			ctx->stats_h264_wait_ns / 1e9,
			(unsigned long long)ctx->stats_h264_async);
	}
	detach_owned_surfaces(ctx);
	free(ctx->slice_data);
	free(ctx->au_data);
	free(ctx->av1.data);
	free(ctx->av1.pending_data);
	free(ctx->av1.au);
	pthread_mutex_destroy(&ctx->mutex);
	free(ctx);
}

void
iris_decode_setup(struct iris_decode_ctx *ctx, unsigned int width,
		  unsigned int height, VAProfile profile)
{
	ctx->width = width;
	ctx->height = height;
	ctx->profile = profile;
	ctx->cap_pixfmt = profile == VAProfileHEVCMain10 ||
		profile == VAProfileVP9Profile2 ? V4L2_PIX_FMT_P010 :
		V4L2_PIX_FMT_NV12;
	switch (profile) {
	case VAProfileHEVCMain:
	case VAProfileHEVCMain10:
		ctx->out_pixfmt = V4L2_PIX_FMT_HEVC;
		break;
	case VAProfileVP9Profile0:
	case VAProfileVP9Profile1:
	case VAProfileVP9Profile2:
	case VAProfileVP9Profile3:
		ctx->out_pixfmt = V4L2_PIX_FMT_VP9;
		break;
	case VAProfileAV1Profile0:
		/* Profile 0 is 8-bit 4:2:0 only; Profile 1/2 (10/12-bit) are not
		 * advertised because the kernel exposes AV1 Main only. */
		ctx->out_pixfmt = V4L2_PIX_FMT_AV1;
		ctx->cap_pixfmt = V4L2_PIX_FMT_NV12;
		break;
	default:
		ctx->out_pixfmt = V4L2_PIX_FMT_H264;
		break;
	}
}

void
iris_decode_set_surfaces(struct iris_decode_ctx *ctx, struct iris_surfs *t)
{
	ctx->surfs = t;
	ctx->vk_copy = t ? t->vk_copy : NULL;
}

static unsigned int
direct_collect_surfaces(struct iris_decode_ctx *ctx)
{
	unsigned int i, count = 0;

	for (i = 0; i < (unsigned int)ctx->surfs->n &&
	     count < ARRAY_SIZE(ctx->direct); i++) {
		struct iris_surface *s = &ctx->surfs->s[i];

		if (s->sw != ctx->width || s->sh != ctx->height ||
		    s->fourcc != ctx->cap_pixfmt || s->bfd < 0 ||
		    (s->owner && s->owner != ctx))
			continue;
		ctx->direct[count].id = s->id;
		ctx->direct[count].fd = s->bfd;
		ctx->direct[count].size = s->bsize;
		count++;
	}
	ctx->direct_count = count;
	return count;
}

int
iris_decode_set_render_targets(struct iris_decode_ctx *ctx,
			       const VASurfaceID *targets, unsigned int count)
{
	const char *slots_env;
	unsigned int i, direct_count = 0;

	if (!getenv("IRIS_DIRECT_CAPTURE") ||
	    ctx->out_pixfmt != V4L2_PIX_FMT_HEVC)
		return 0;
	if (targets && count >= 4) {
		if (count > ARRAY_SIZE(ctx->direct))
			return -EINVAL;
		for (i = 0; i < count; i++) {
			struct iris_surface *s = find_surface(ctx, targets[i]);

			if (!s || s->bfd < 0 || s->fourcc != ctx->cap_pixfmt)
				return -EINVAL;
			ctx->direct[i].id = targets[i];
			ctx->direct[i].fd = s->bfd;
			ctx->direct[i].size = s->bsize;
		}
		direct_count = count;
		ctx->direct_requested_count = count;
	} else {
		direct_count = direct_collect_surfaces(ctx);
		/* A dynamic libva pool exposes only one new target at a time.  Its
		 * eventual cycle length cannot be inferred from that first request,
		 * while a stateful decoder needs the complete CAPTURE queue ordering
		 * up front.  Keep the experiment deterministic by requiring its ring
		 * size; fixed-pool clients already supply all render targets above. */
		slots_env = getenv("IRIS_DIRECT_CAPTURE_SLOTS");
		if (slots_env && *slots_env) {
			char *end;
			unsigned long slots = strtoul(slots_env, &end, 10);

			if (*end || slots < 4 ||
			    slots > ARRAY_SIZE(ctx->direct))
				return -EINVAL;
			ctx->direct_requested_count = (unsigned int)slots;
			if (direct_count > slots)
				direct_count = (unsigned int)slots;
		} else {
			fprintf(stderr,
				"iris-vaapi: direct CAPTURE needs a fixed render-target pool; dynamic clients must set IRIS_DIRECT_CAPTURE_SLOTS\n");
			return -ENOTSUP;
		}
	}
	ctx->direct_count = direct_count;
	ctx->direct_capture = 1;
	fprintf(stderr,
		"iris-vaapi: experimental direct CAPTURE ring: %s%u/%u surfaces\n",
		direct_count < ctx->direct_requested_count ? "deferred " : "",
		direct_count, ctx->direct_requested_count);
	return 0;
}

int
iris_decode_create_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	/* Legacy entry point kept for local tests: allocate into whatever
	 * registry the engine is attached to. */
	if (!ctx || !ctx->surfs)
		return -1;
	return iris_surfs_alloc(ctx->surfs, id, ctx->width, ctx->height,
				 ctx->cap_pixfmt);
}

void
iris_decode_destroy_surface(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	if (!ctx || !ctx->surfs)
		return;
	iris_surfs_free(ctx->surfs, id);
}

/* Update the coded dimensions of a context that has not started decoding
 * yet (Chrome recreates contexts on resolution changes). */
void
iris_decode_reconfigure(struct iris_decode_ctx *ctx, unsigned int width,
			unsigned int height)
{
	if (!ctx || ctx->dec_open || !width || !height)
		return;
	ctx->width = width;
	ctx->height = height;
}

/* Tear down the firmware session and all stream state (Chrome Flush/Reset,
 * seeks).  Surfaces and their backings are preserved so frames already
 * exported to the client stay valid. */
void
iris_decode_reset(struct iris_decode_ctx *ctx)
{
	if (!ctx)
		return;
	pthread_mutex_lock(&ctx->mutex);
	if (ctx->dec_open) {
		finish_pending_writes(ctx);
		forget_vk_capture_buffers(ctx);
		v4l2_dec_close(&ctx->dec);
		ctx->dec_open = 0;
	}
	reset_stream_state(ctx);
	pthread_mutex_unlock(&ctx->mutex);
}

/* ---- Display-level surface registry ---- */

struct iris_surfs *
iris_surfs_create(void)
{
	struct iris_surfs *t = calloc(1, sizeof(*t));

	if (t && getenv("IRIS_VULKAN_COPY"))
		t->vk_copy = iris_vk_copy_create();
	return t;
}

void
iris_surfs_destroy(struct iris_surfs *t)
{
	int i;

	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		struct iris_surface *s = &t->s[i];

		if (s->owner)
			pthread_mutex_lock(&s->owner->mutex);
		if (s->owner)
			(void)finish_vk_surface(s->owner, s->id, 1);
		if (s->owner)
			iris_vk_copy_forget(s->owner->vk_copy,
					    s->backing_serial);
		if (s->owner && s->owner->dec_open &&
		    (s->write_started || s->fence_token))
			surface_finish_write(&s->owner->dec, s);
		munmap(t->s[i].bmap, t->s[i].bsize);
		close(t->s[i].bfd);
		if (s->owner)
			pthread_mutex_unlock(&s->owner->mutex);
	}
	iris_vk_copy_destroy(t->vk_copy);
	free(t);
}

int
iris_surfs_alloc(struct iris_surfs *t, VASurfaceID id,
		 unsigned int width, unsigned int height, unsigned int fourcc)
{
	struct iris_surface *s;
	unsigned int size;
	int heap, bfd;
	void *map;

	if (!t || t->n >= IRIS_MAX_SURFACES)
		return -1;

	/* Stable, exportable backing buffer independent of any V4L2 session.
	 * Prefer a real DMA-heap buffer so the exported fd can be imported by
	 * GPU clients (Chrome/EGL); fall back to a plain memfd when the heap
	 * node is root-only, which keeps local tests and CPU readback working.
	 * Size with the layout Iris produces for linear NV12/P010 (128-byte
	 * NV12 stride, 256-byte P010 stride, 32-aligned luma height). */
	size = surface_pitch(width, fourcc) * ALIGN_TO(height, 32) * 3 / 2;
	DBG("[surf] id=%u size=%u w=%u h=%u fourcc=%#x\n",
	    id, size, width, height, fourcc);
	heap = open("/dev/dma_heap/system", O_RDWR);
	if (heap >= 0) {
		bfd = dma_heap_alloc(heap, size);
		close(heap);
	} else {
		fprintf(stderr, "[surf] dma_heap unavailable (%s); "
			"using memfd backing (not GPU-importable)\n",
			strerror(errno));
		bfd = memfd_alloc(size);
	}
	if (bfd < 0) {
		perror("[surf] alloc backing");
		return -1;
	}
	map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, bfd, 0);
	if (map == MAP_FAILED) {
		perror("[surf] mmap");
		close(bfd);
		return -1;
	}

	s = &t->s[t->n++];
	s->id = id;
	s->bfd = bfd;
	s->bmap = map;
	s->bsize = size;
	s->sw = width;
	s->sh = height;
	s->fourcc = fourcc;
	s->decoded = 0;
	s->queued = 0;
	s->exported = 0;
	s->write_started = 0;
	s->fence_token = 0;
	s->generation = 0;
	s->backing_serial = __atomic_add_fetch(&g_buffer_serial, 1,
					       __ATOMIC_RELAXED);
	s->owner = NULL;
	surface_fill_black(bfd, map, surface_pitch(width, fourcc), height,
			   fourcc);
	return 0;
}

void
iris_surfs_free(struct iris_surfs *t, VASurfaceID id)
{
	int i;

	if (!t)
		return;
	for (i = 0; i < t->n; i++) {
		struct iris_surface *s = &t->s[i];

		if (s->id != id)
			continue;
		if (s->owner)
			pthread_mutex_lock(&s->owner->mutex);
		if (s->owner)
			(void)finish_vk_surface(s->owner, s->id, 1);
		if (s->owner)
			iris_vk_copy_forget(s->owner->vk_copy,
					    s->backing_serial);
		if (s->owner && s->owner->dec_open &&
		    (s->write_started || s->fence_token))
			surface_finish_write(&s->owner->dec, s);
		munmap(s->bmap, s->bsize);
		close(s->bfd);
		if (s->owner)
			pthread_mutex_unlock(&s->owner->mutex);
		t->s[i] = t->s[t->n - 1];
		t->n--;
		return;
	}
}

/* Actual NV12 layout of @s: what its owning engine negotiated for CAPTURE,
 * or the aligned coded size before a session exists.  Export/derive must
 * describe buffers with these values, never with assumptions, or clients
 * read garbled rows. */
static void
surface_layout(const struct iris_surface *s, unsigned int *pitch,
	       unsigned int *width, unsigned int *height)
{
	unsigned int p = surface_pitch(s->sw, s->fourcc);
	unsigned int w = s->sw;
	unsigned int h = ALIGN_TO(s->sh, 32);

	if (s->owner && s->owner->dec_open &&
	    s->owner->dec.cap_fmt.fmt.pix_mp.width &&
	    s->owner->dec.cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline) {
		p = s->owner->dec.cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		w = s->owner->dec.cap_fmt.fmt.pix_mp.width;
		h = s->owner->dec.cap_fmt.fmt.pix_mp.height;
	}
	*pitch = p;
	*width = w;
	*height = h;
}

static int
iris_decode_surface_ready(struct iris_decode_ctx *ctx, VASurfaceID id);

int
iris_surfs_sync(struct iris_surfs *t, VASurfaceID id)
{
	struct iris_surface *s = surfs_find(t, id);

	/* Never-queued and already-decoded surfaces succeed without
	 * draining anything: Chrome syncs freshly allocated pool surfaces
	 * before exporting them and must not get spurious timeouts. */
	if (!s)
		return -EINVAL;
	if (!s->queued || s->decoded)
		return 0;
	/* Drain whichever engine queued this picture; with one engine per
	 * context that is exactly the context still holding the stream. */
	if (s->owner) {
		int r = iris_decode_sync(s->owner, id);

		DBG("[surfs_sync] id=%u -> engine sync r=%d\n", id, r);
		return r;
	}
	return 0;
}

int
iris_surfs_ready(struct iris_surfs *t, VASurfaceID id)
{
	struct iris_surface *s = surfs_find(t, id);

	return s ? s->decoded : 0;
}

int
iris_surfs_valid(struct iris_surfs *t, VASurfaceID id)
{
	return surfs_find(t, id) != NULL;
}

int
iris_surfs_export(struct iris_surfs *t, VASurfaceID id, int *fd,
		  unsigned int *pitch, unsigned int *size,
		  unsigned int *width, unsigned int *height,
		  unsigned int *fourcc)
{
	struct iris_surface *s = surfs_find(t, id);
	int exported_fd;
	unsigned int p, w, h;

	if (!s)
		return -1;
	surface_layout(s, &p, &w, &h);
	/* vaExportSurfaceHandle transfers ownership of every returned object
	 * fd to the caller.  Keep the driver's backing fd private: Chrome
	 * closes the exported fd after importing it, and returning s->bfd
	 * directly caused a double-close/FD-reuse crash in the GPU process. */
	exported_fd = fcntl(s->bfd, F_DUPFD_CLOEXEC, 0);
	if (exported_fd < 0)
		return -1;
	/* If this backing is reused for a later picture, legacy Adreno may have
	 * imported it before the new reservation fence was attached. */
	s->exported = 1;
	*fd = exported_fd;
	*pitch = p;
	*size = s->bsize;
	*width = w;
	*height = h;
	*fourcc = s->fourcc;
	return 0;
}

int
iris_surfs_buffer(struct iris_surfs *t, VASurfaceID id, void **mem,
		  unsigned int *pitch, unsigned int *size,
		  unsigned int *width, unsigned int *height,
		  unsigned int *fourcc)
{
	struct iris_surface *s = surfs_find(t, id);
	unsigned int p, w, h;

	if (!s)
		return -1;
	surface_layout(s, &p, &w, &h);
	*mem = s->bmap;
	*pitch = p;
	*size = s->bsize;
	*width = w;
	*height = h;
	*fourcc = s->fourcc;
	return 0;
}

static int
ensure_decoder(struct iris_decode_ctx *ctx)
{
	int ret;

	if (ctx->dec_open) {
		/* After an EOS flush the firmware is done; a client that keeps
		 * decoding (Chrome flush/reset, looped playback) needs a fresh
		 * session.  Preserve the picture already collected for this
		 * vaEndPicture call while clearing the old firmware bookkeeping. */
		if (ctx->eos_sent) {
			finish_pending_writes(ctx);
			forget_vk_capture_buffers(ctx);
			v4l2_dec_close(&ctx->dec);
			ctx->dec_open = 0;
			reset_decoder_session_state(ctx);
		} else {
			return 0;
		}
	}
	ret = v4l2_dec_open(&ctx->dec, NULL, ctx->width,
			    ctx->height, ctx->out_pixfmt, ctx->cap_pixfmt);
	if (ret)
		return ret;
	if (ctx->direct_capture) {
		int fds[IRIS_MAX_SURFACES];
		size_t sizes[IRIS_MAX_SURFACES];
		unsigned int i;

		direct_collect_surfaces(ctx);
		if (ctx->direct_count < ctx->direct_requested_count) {
			v4l2_dec_close(&ctx->dec);
			return -EINVAL;
		}
		ctx->direct_count = ctx->direct_requested_count;
		for (i = 0; i < ctx->direct_count; i++) {
			fds[i] = ctx->direct[i].fd;
			sizes[i] = ctx->direct[i].size;
		}
		ret = v4l2_dec_set_capture_dmabufs(&ctx->dec, fds, sizes,
						     ctx->direct_count);
		if (ret) {
			v4l2_dec_close(&ctx->dec);
			return ret;
		}
	}
	ctx->dec_open = 1;
	return 0;
}

/* Drop a live firmware session at a random-access boundary without touching
 * the picture currently being assembled.  Chromium's decoder Reset() does
 * not issue any VA-API operation, so a seek otherwise leaves old DPB and
 * CAPTURE work in the stateful V4L2 session until the first post-seek frame
 * arrives. */
static void
restart_decoder_session(struct iris_decode_ctx *ctx)
{
	int i;

	if (!ctx->dec_open)
		return;
	finish_pending_writes(ctx);
	forget_vk_capture_buffers(ctx);
	v4l2_dec_close(&ctx->dec);
	ctx->dec_open = 0;
	/* Pending pictures from the abandoned stream keep their stable backing,
	 * but no longer have firmware work that a later vaSyncSurface can drain. */
	if (ctx->surfs) {
		for (i = 0; i < ctx->surfs->n; i++) {
			struct iris_surface *s = &ctx->surfs->s[i];

			if (s->owner == ctx && s->queued && !s->decoded)
				s->queued = 0;
		}
	}
	reset_decoder_session_state(ctx);
}

static int
direct_surface_index(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	unsigned int i;

	for (i = 0; i < ctx->direct_count; i++)
		if (ctx->direct[i].id == id)
			return (int)i;
	return -1;
}

static void
hevc_pending_add(struct iris_decode_ctx *ctx, int32_t poc, VASurfaceID target,
		 uint64_t generation)
{
	unsigned int n = (unsigned int)ARRAY_SIZE(ctx->hevc_ring);
	unsigned int slot = (ctx->hevc_ring_head + ctx->hevc_ring_len) % n;

	ctx->hevc_ring[slot].poc = poc;
	ctx->hevc_ring[slot].target = target;
	ctx->hevc_ring[slot].generation = generation;
	if (ctx->hevc_ring_len < n) {
		ctx->hevc_ring_len++;
	} else {
		/* Pathologically deep pipeline: drop the oldest mapping
		 * instead of failing the picture submission. */
		ctx->hevc_ring_head = (ctx->hevc_ring_head + 1) % n;
	}
}

static int
hevc_pending_take(struct iris_decode_ctx *ctx, VASurfaceID *target,
		  uint64_t *generation)
{
	unsigned int n = (unsigned int)ARRAY_SIZE(ctx->hevc_ring);

	if (!ctx->hevc_ring_len)
		return -1;
	*target = ctx->hevc_ring[ctx->hevc_ring_head].target;
	*generation = ctx->hevc_ring[ctx->hevc_ring_head].generation;
	ctx->hevc_ring_head = (ctx->hevc_ring_head + 1) % n;
	ctx->hevc_ring_len--;
	return 0;
}


/* Assign one dequeued frame to its surface.  H.264/VP9 use the timestamp
 * propagated by the firmware.  Iris does not preserve usable HEVC timestamps,
 * so HEVC matches display-order CAPTURE frames against pending picture POCs.
 * Returns the surface id, or -1 if unknown. */
static int
assign_frame(struct iris_decode_ctx *ctx, const struct v4l2_dec_frame *frame)
{
	VASurfaceID id;
	uint64_t generation;
	struct iris_surface *s;

	if (frame->bytesused && ctx->stats_enabled)
		ctx->stats_capture_frames++;

	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC ||
	    ctx->out_pixfmt == V4L2_PIX_FMT_AV1) {
		/* Neither HEVC nor AV1 firmware propagates usable per-frame
		 * timestamps; with decode-order output the CAPTURE frames
		 * complete in submission order, so a strict FIFO maps them
		 * back to their render targets. */
		if (hevc_pending_take(ctx, &id, &generation)) {
			v4l2_dec_qcap_idx(&ctx->dec, frame->index);
			return -EIO;
		}
	} else {
		uint64_t frame_seq, slot;

		if (frame->timestamp < 1000000000ULL) {
			/* Decode-order output: the firmware occasionally
			 * completes a capture buffer with no input
			 * timestamp (internal/dummy picture).  Recycle it
			 * like an unknown picture -- treating it as fatal
			 * starves the CAPTURE queue and wedges decoding. */
			DBG("[assign] ts=0 recycle idx=%u\n", frame->index);
			v4l2_dec_qcap_idx(&ctx->dec, frame->index);
			return 0;
		}
		frame_seq = (frame->timestamp / 1000000000ULL) - 1000;
		slot = frame_seq % ARRAY_SIZE(ctx->target_ring);
		if (!ctx->target_ring[slot].used ||
		    ctx->target_ring[slot].seq != frame_seq) {
			/* Unknown/stale picture.  The buffer MUST be recycled
			 * or the CAPTURE queue starves and decoding wedges. */
			DBG("[assign] UNKNOWN ts=%llu seq=%llu slot=%llu used=%d rseq=%llu\n",
			    (unsigned long long)frame->timestamp,
			    (unsigned long long)frame_seq,
			    (unsigned long long)slot,
			    ctx->target_ring[slot].used,
			    (unsigned long long)ctx->target_ring[slot].seq);
			v4l2_dec_qcap_idx(&ctx->dec, frame->index);
			return 0;
		}
		id = ctx->target_ring[slot].target;
		generation = ctx->target_ring[slot].generation;
		ctx->target_ring[slot].used = 0;
	}
	s = find_surface(ctx, id);

	if (!s) {
		DBG("[assign] NO SURFACE id=%u\n", id);
		v4l2_dec_qcap_idx(&ctx->dec, frame->index);
		return 0;
	}
	/* A VA client may recycle a render target as soon as it drops the old
	 * output frame, while legacy Iris firmware can still have that picture
	 * queued internally (up to its display/decode hold depth).  Do not let
	 * such a late CAPTURE buffer overwrite the newer picture already mapped
	 * to the same stable backing, nor signal the newer picture's fence. */
	if (s->generation != generation) {
		DBG("[assign] STALE id=%u generation=%llu current=%llu\n", id,
		    (unsigned long long)generation,
		    (unsigned long long)s->generation);
		v4l2_dec_qcap_idx(&ctx->dec, frame->index);
		return 0;
	}
	if (ctx->direct_capture) {
		VASurfaceID actual;

		if (frame->index >= ctx->direct_count) {
			ctx->direct_error = -ERANGE;
			return -1;
		}
		actual = ctx->direct[frame->index].id;
		if (actual != id) {
			fprintf(stderr,
				"direct CAPTURE order mismatch: slot=%u surface=%u expected=%u\n",
				frame->index, actual, id);
			ctx->direct_error = -EIO;
			return -1;
		}
		surface_finish_write(&ctx->dec, s);
		s->decoded = 1;
		s->queued = 1;
		s->owner = ctx;
		ctx->stats_direct_frames++;
		return id;
	}
	/* Copy the decoded frame into the surface's stable DMA-heap backing so
	 * buffers exported before decoding stay valid, then recycle the
	 * firmware buffer. */
	if (frame->bytesused > s->bsize) {
		fprintf(stderr,
			"[copy] WARNING frame %u bytes > backing %u; "
			"dropping surface %u\n",
			frame->bytesused, s->bsize, id);
		surface_finish_write(&ctx->dec, s);
		v4l2_dec_qcap_idx(&ctx->dec, frame->index);
		return -1;
	} else if (frame->bytesused) {
		int copy_ret = surface_vk_submit(ctx, s, frame);

		if (copy_ret == 0)
			return id;
		if (copy_ret < 0)
			copy_ret = surface_copy(ctx, s, frame->mem,
						frame->bytesused);
		else
			copy_ret = 0;

		if (copy_ret < 0) {
			fprintf(stderr,
				"[copy] DMA-BUF sync/copy failed for surface %u: %s\n",
				id, strerror(-copy_ret));
			v4l2_dec_qcap_idx(&ctx->dec, frame->index);
			return -1;
		}
	} else {
		surface_finish_write(&ctx->dec, s);
	}
	{
		int ret = v4l2_dec_qcap_idx(&ctx->dec, frame->index);

		if (ret)
			return ret;
	}
	s->decoded = 1;
	s->queued = 1;
	s->owner = ctx;
	return id;
}

static int drain_available(struct iris_decode_ctx *ctx);

/* Wait for one already-submitted render target without flushing the stream.
 * The V4L2 session requests decode-order output, so firmware can complete the
 * current target without needing another picture to be queued.
 * This is used before vaEndPicture returns: the ANGLE/GL import path on legacy
 * Adreno does not reliably wait for reservation fences attached after the
 * DMA-BUF was imported, and can otherwise sample that surface's old pixels. */
static int
wait_surface_ready(struct iris_decode_ctx *ctx, VASurfaceID id, int deadline)
{
	while (deadline-- > 0) {
		struct iris_surface *s;
		struct v4l2_dec_frame frame;
		int changed, ret;

		ret = drain_available(ctx);
		if (ret)
			return ret;
		ret = finish_vk_surface(ctx, id, 1);
		if (ret && ret != -EAGAIN)
			return ret;
		s = find_surface(ctx, id);
		if (s && s->decoded) {
			if (ctx->av1.feed_ns && ctx->out_pixfmt == V4L2_PIX_FMT_AV1)
				DBG("[wait] target=%u firmware %llu us\n", id,
				    (unsigned long long)
				    ((monotonic_ns() - ctx->av1.feed_ns) / 1000));
			return 0;
		}

		/* After an EOS drain (flush, seek or stall recovery) the
		 * firmware session is finished: no further CAPTURE work can
		 * appear.  Treat an undecoded surface as ready (the usual
		 * sync no-op) rather than touching the dead session, whose
		 * DQBUF fails with EPIPE.  The next decode reopens a fresh
		 * session via ensure_decoder(). */
		if (ctx->eos_sent || !ctx->dec_started)
			return 0;

		ret = v4l2_dec_poll_cap(&ctx->dec, 20);
		if (ret < 0)
			return ret;
		if (!ret)
			continue;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
		if (ret == -EAGAIN)
			continue;
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		ret = assign_frame(ctx, &frame);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
	}
	return -ETIMEDOUT;
}

/* Force the firmware to release the picture it is holding.  The stateful
 * decoder keeps the most recent picture until the next access unit (or an
 * EOS marker) arrives; without this, vaSyncSurface on the last picture
 * always times out. */
static int
iris_decode_flush_impl(struct iris_decode_ctx *ctx)
{
	struct v4l2_dec_frame frame;
	int ret, deadline = 100;

	if (!ctx->dec_open || !ctx->dec_started || ctx->eos_sent)
		return ctx->fatal_error;

	ret = v4l2_dec_flush(&ctx->dec);
	if (ret)
		return ret;
	ctx->eos_sent = 1;

	/* Drain decoded pictures until the empty V4L2_BUF_FLAG_LAST marker is
	 * dequeued.  Pictures preceding that marker are assigned normally. */
	while (deadline-- > 0) {
		int changed;

		ret = v4l2_dec_poll_cap(&ctx->dec, 20);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			continue;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
		if (ret == -EAGAIN)
			continue;
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		DBG("[flush] got ts=%llu flags=0x%x\n",
		    (unsigned long long)frame.timestamp, frame.flags);
		if (frame.bytesused && assign_frame(ctx, &frame) < 0) {
			ctx->fatal_error = -EIO;
			return ctx->fatal_error;
		}
		if (ret == 1)
			break;
	}
	if (ctx->dec.eos) {
		ret = reap_vk_copies(ctx, 1);
		return ret;
	}
	return -ETIMEDOUT;
}

/* Recovery for a firmware stall: the stateful decoder occasionally holds a
 * picture past the readiness-wait deadline (observed as a rare start-up race
 * in Firefox's synchronous decode-sync-decode pattern, roughly every other
 * session).  An EOS drain forces the firmware to release every held picture,
 * which completes the stalled target; ctx->eos_sent then makes the next
 * ensure_decoder() reopen a fresh session, exactly like looped playback.
 * Returns 0 when @id became ready, -EAGAIN when it did not. */
static int
recover_stalled_target(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	struct iris_surface *s;
	int deadline = 150, changed, ret;

	s = find_surface(ctx, id);
	if (!s || s->decoded || ctx->eos_sent || ctx->fatal_error ||
	    !ctx->dec_open || !ctx->dec_started)
		return -EAGAIN;

	DBG("[stall] target=%u: EOS-kicking stalled firmware\n", id);
	ret = v4l2_dec_flush(&ctx->dec);
	if (ret)
		return ret;
	ctx->eos_sent = 1;

	while (deadline-- > 0) {
		ret = v4l2_dec_poll_cap(&ctx->dec, 20);
		if (ret < 0)
			return ret;
		if (!ret)
			continue;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		{
			struct v4l2_dec_frame frame;

			ret = v4l2_dec_dqcap(&ctx->dec, &frame);
			if (ret == -EAGAIN)
				continue;
			if (ret < 0)
				return ret;
			DBG("[stall] got ts=%llu flags=0x%x\n",
			    (unsigned long long)frame.timestamp, frame.flags);
			if (frame.bytesused && assign_frame(ctx, &frame) < 0)
				return -EIO;
			if (ret == 1)
				break;	/* FLAG_LAST */
		}
		s = find_surface(ctx, id);
		if (s && s->decoded)
			break;
	}
	reap_vk_copies(ctx, 1);
	s = find_surface(ctx, id);
	if (s && s->decoded)
		return 0;
	DBG("[stall] target=%u still not ready after EOS kick\n", id);
	return -EAGAIN;
}

/* Establish a strict stream boundary for Chromium seeks.  Chromium does
 * not forward Decoder::Reset() through VA-API, so the first post-seek key frame
 * is the earliest point where the driver can act.  Finish the complete old
 * OUTPUT/CAPTURE pipeline and observe LAST before closing it; only then may the
 * already assembled seek key frame be submitted to a fresh firmware session.
 * Preserve the private sequence epoch across every codec restart; legacy VPU5
 * VP9 has proved sensitive to a timestamp rewind across a context reused by
 * Chromium. */
static int
drain_and_restart_seek(struct iris_decode_ctx *ctx)
{
	uint64_t next_seq = ctx->seq;
	unsigned int pending = 0, i;
	int ret;

	for (i = 0; i < ARRAY_SIZE(ctx->target_ring); i++)
		pending += ctx->target_ring[i].used != 0;
	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC ||
	    ctx->out_pixfmt == V4L2_PIX_FMT_AV1)
		pending = ctx->hevc_ring_len;
	DBG("[seek] draining %u pre-seek mappings codec=0x%x\n",
	    pending, ctx->out_pixfmt);
	ret = iris_decode_flush_impl(ctx);
	if (ret) {
		DBG("[seek] pre-seek drain failed codec=0x%x: %d\n",
		    ctx->out_pixfmt, ret);
		return ret;
	}
	restart_decoder_session(ctx);
	ctx->seq = next_seq;
	ctx->vp9_seek_barrier = ctx->out_pixfmt == V4L2_PIX_FMT_VP9;
	DBG("[seek] old session complete codec=0x%x; restart at seq=%llu\n",
	    ctx->out_pixfmt, (unsigned long long)ctx->seq);
	return 0;
}

int
iris_decode_flush(struct iris_decode_ctx *ctx)
{
	int ret;

	pthread_mutex_lock(&ctx->mutex);
	ret = iris_decode_flush_impl(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

/* Non-blocking drain of whatever frames are ready. */
static int
drain_available(struct iris_decode_ctx *ctx)
{
	int copy_ret;

	if (ctx->fatal_error)
		return ctx->fatal_error;
	copy_ret = reap_vk_copies(ctx, 0);
	if (copy_ret < 0)
		return copy_ret;
	if (ctx->eos_sent)
		return 0;

	for (;;) {
		struct v4l2_dec_frame frame;
		int changed, ret;

		ret = v4l2_dec_poll(&ctx->dec, 0);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			break;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
		if (ret == -EAGAIN)
			break;
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		ret = assign_frame(ctx, &frame);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (ctx->direct_error)
			return ctx->direct_error;
		ret = reap_vk_copies(ctx, 0);
		if (ret < 0)
			return ret;
	}
	return 0;
}

static int
iris_decode_begin_impl(struct iris_decode_ctx *ctx, VASurfaceID target)
{
	struct iris_surface *s = find_surface(ctx, target);
	int direct_index = ctx->direct_capture ?
		direct_surface_index(ctx, target) : -1;

	if (ctx->direct_error)
		return ctx->direct_error;
	if (ctx->fatal_error)
		return ctx->fatal_error;
	if (!s)
		return -EINVAL;
	/* Never recycle a target while an earlier GPU copy still writes it. */
	if (finish_vk_surface(ctx, target, 1) < 0)
		return -EIO;
	/* Chrome creates its VAContext before it asks for the first surface.  The
	 * opt-in pool preallocation has happened by the first BeginPicture, so bind
	 * the deferred fixed ring here, before V4L2 is opened. */
	if (ctx->direct_capture && direct_index < 0 && !ctx->dec_open) {
		direct_collect_surfaces(ctx);
		if (ctx->direct_count >= ctx->direct_requested_count)
			ctx->direct_count = ctx->direct_requested_count;
		direct_index = direct_surface_index(ctx, target);
	}
	if (ctx->direct_capture && direct_index < 0)
		return -EINVAL;

	DBG("[begin] target=%u decoded=%d queued=%d generation=%llu\n",
	    target, s ? s->decoded : -1, s ? s->queued : -1,
	    (unsigned long long)(s ? s->generation : 0));
	ctx->current_target = target;
	/* VA clients reuse render targets.  A surface that held an earlier
	 * picture must become pending again, otherwise vaSyncSurface can return
	 * the stale backing before the newly decoded picture is copied into it. */
	s->generation++;
	ctx->current_generation = s->generation;
	s->decoded = 0;
	s->queued = 0;
	ctx->have_pic = 0;
	ctx->have_hevc_pic = 0;
	ctx->slice_len = 0;
	ctx->refs_l0 = 0;
	ctx->refs_l1 = 0;
	ctx->h264_slice_count = 0;
	ctx->hevc_slice_count = 0;
	ctx->hevc_slice_next = 0;
	ctx->hevc_pps_id = -1;
	ctx->hevc_rewritten = 0;
	if (ctx->out_pixfmt == V4L2_PIX_FMT_AV1) {
		ctx->av1.have_pic = 0;
		ctx->av1.n_slices = 0;
		ctx->av1.data_len = 0;
		ctx->av1.blob_ready = 0;
	}
	return 0;
}

int
iris_decode_begin(struct iris_decode_ctx *ctx, VASurfaceID target)
{
	int ret;

	pthread_mutex_lock(&ctx->mutex);
	ret = iris_decode_begin_impl(ctx, target);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

int
iris_decode_slice_params(struct iris_decode_ctx *ctx,
			 const VASliceParameterBufferH264 *sp)
{
	if (sp->num_ref_idx_l0_active_minus1 > ctx->refs_l0)
		ctx->refs_l0 = sp->num_ref_idx_l0_active_minus1;
	if (sp->num_ref_idx_l1_active_minus1 > ctx->refs_l1)
		ctx->refs_l1 = sp->num_ref_idx_l1_active_minus1;
	if (ctx->h264_slice_count >= ARRAY_SIZE(ctx->h264_slices))
		return -1;
	memcpy(&ctx->h264_slices[ctx->h264_slice_count].param, sp,
	       sizeof(*sp));
	ctx->h264_slice_count++;
	return 0;
}

int
iris_decode_picture(struct iris_decode_ctx *ctx,
		    const VAPictureParameterBufferH264 *pic)
{
	memcpy(&ctx->pic, pic, sizeof(*pic));
	ctx->have_pic = 1;
	return 0;
}

int
iris_decode_h264_iq_matrix(struct iris_decode_ctx *ctx,
			   const VAIQMatrixBufferH264 *iq)
{
	if (!iq)
		return -EINVAL;
	memcpy(&ctx->h264_iq, iq, sizeof(*iq));
	ctx->have_h264_iq = 1;
	return 0;
}

int
iris_decode_hevc_picture(struct iris_decode_ctx *ctx,
			 const VAPictureParameterBufferHEVC *pic)
{
	memcpy(&ctx->hevc_pic, pic, sizeof(*pic));
	ctx->have_hevc_pic = 1;
	return 0;
}

int
iris_decode_hevc_iq_matrix(struct iris_decode_ctx *ctx,
			   const VAIQMatrixBufferHEVC *iq)
{
	if (!iq)
		return -EINVAL;
	memcpy(&ctx->hevc_iq, iq, sizeof(*iq));
	ctx->have_hevc_iq = 1;
	return 0;
}

int
iris_decode_hevc_slice_params(struct iris_decode_ctx *ctx,
			      const VASliceParameterBufferHEVC *sp)
{
	if (sp->num_ref_idx_l0_active_minus1 > ctx->refs_l0)
		ctx->refs_l0 = sp->num_ref_idx_l0_active_minus1;
	if (sp->num_ref_idx_l1_active_minus1 > ctx->refs_l1)
		ctx->refs_l1 = sp->num_ref_idx_l1_active_minus1;
	/* VA-API may submit one parameter buffer containing many slice elements.
	 * Keep every range; retaining only the last element corrupts multi-slice
	 * 4K/8K pictures. */
	if (ctx->hevc_slice_count >= 128)
		return -1;
	memcpy(&ctx->hevc_slices[ctx->hevc_slice_count].param, sp, sizeof(*sp));
	ctx->hevc_slice_count++;
	return 0;
}

static int
has_start_code(const uint8_t *p, size_t len)
{
	if (len >= 4 && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1)
		return 4;
	if (len >= 3 && p[0] == 0 && p[1] == 0 && p[2] == 1)
		return 3;
	return 0;
}

/* Identify independently decodable pictures that can safely start a fresh
 * stateful V4L2 session.  This is also the only signal the VA driver sees for
 * Chromium seeks because the software decoder reset is not forwarded through
 * libva. */
static int
is_random_access_picture(const struct iris_decode_ctx *ctx)
{
	size_t i = 0;

	if (ctx->out_pixfmt == V4L2_PIX_FMT_AV1) {
		/* VA carries the parsed frame header, so no bitstream probing is
		 * needed: a KEY_FRAME is always a safe restart point. */
		if (!ctx->av1.have_pic)
			return 0;
		return ctx->av1.pic.pic_info_fields.bits.frame_type == 0;
	}

	if (ctx->out_pixfmt == V4L2_PIX_FMT_VP9) {
		unsigned int profile, show_existing, frame_type;
		uint8_t b;

		if (!ctx->slice_len)
			return 0;
		b = ctx->slice_data[0];
		/* VP9's uncompressed header is packed most-significant bit first.
		 * Profile 0 key/inter frames commonly start with 0x82/0x86: the
		 * frame_marker is bits 7..6 and frame_type is bit 2. */
		if ((b >> 6) != 2) /* frame_marker */
			return 0;
		profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
		show_existing = (b >> (profile == 3 ? 2 : 3)) & 1;
		if (show_existing)
			return 0;
		frame_type = (b >> (profile == 3 ? 1 : 2)) & 1;
		return frame_type == 0;
	}

	while (i + 3 < ctx->slice_len) {
		int sc = has_start_code(ctx->slice_data + i,
					ctx->slice_len - i);
		unsigned int type;

		if (!sc) {
			i++;
			continue;
		}
		i += (size_t)sc;
		if (i >= ctx->slice_len)
			break;
		if (ctx->out_pixfmt == V4L2_PIX_FMT_H264) {
			type = ctx->slice_data[i] & 0x1f;
			if (type == 5) /* IDR */
				return 1;
		} else if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC) {
			type = (ctx->slice_data[i] >> 1) & 0x3f;
			if (type >= 16 && type <= 23) /* BLA/IDR/CRA */
				return 1;
		}
		while (i < ctx->slice_len &&
		       !has_start_code(ctx->slice_data + i,
					ctx->slice_len - i))
			i++;
	}
	return 0;
}

/* Build the shortest VP9 show_existing_frame access unit, selecting reference
 * slot zero.  A key frame refreshes all eight slots, so slot zero is valid at
 * the post-seek barrier.  show_existing_frame does not decode or refresh any
 * reference; it only supplies the extra OUTPUT boundary needed to make legacy
 * VPU5 release the preceding real inter frame. */
static size_t
vp9_show_existing_au(const struct iris_decode_ctx *ctx, uint8_t au[2])
{
	unsigned int profile;
	uint8_t b;

	if (!ctx->slice_len)
		return 0;
	b = ctx->slice_data[0];
	if ((b >> 6) != 2)
		return 0;
	profile = ((b >> 5) & 1) | (((b >> 4) & 1) << 1);
	au[0] = 0x80 | (b & 0x30);
	if (profile == 3) {
		/* profile 3 has reserved_zero before show_existing_frame; the
		 * final frame_to_show_map_idx bit spills into the second byte. */
		au[0] |= 0x04;
		au[1] = 0;
		return 2;
	}
	au[0] |= 0x08;
	return 1;
}

/* VA exposes the effective reference counts for each slice.  Those values
 * include slice-header overrides and therefore must not be copied into the
 * PPS on every picture.  Find slices that actually use the PPS defaults and
 * learn the stable values from their VA parameters. */
static int
h264_update_pps_defaults(struct iris_decode_ctx *ctx)
{
	size_t i = 0;
	unsigned int slice = 0;

	while (i + 3 < ctx->slice_len) {
		int sc = has_start_code(ctx->slice_data + i,
					ctx->slice_len - i);
		size_t start, end;
		int mask;
		const VASliceParameterBufferH264 *sp;

		if (!sc) {
			i++;
			continue;
		}
		start = i + sc;
		end = start;
		while (end < ctx->slice_len &&
		       !has_start_code(ctx->slice_data + end,
				       ctx->slice_len - end))
			end++;
		if (end <= start || (ctx->slice_data[start] & 0x1f) < 1 ||
		    (ctx->slice_data[start] & 0x1f) > 5) {
			i = end;
			continue;
		}
		if (slice >= ctx->h264_slice_count)
			return -1;
		sp = &ctx->h264_slices[slice++].param;
		mask = h264_slice_default_ref_mask(ctx->slice_data + start,
						  end - start, &ctx->pic);
		if (mask < 0)
			return -1;
		if (mask & 1)
			ctx->h264_pps_refs_l0 =
				sp->num_ref_idx_l0_active_minus1;
		if (mask & 2)
			ctx->h264_pps_refs_l1 =
				sp->num_ref_idx_l1_active_minus1;
		i = end;
	}
	return slice == ctx->h264_slice_count ? 0 : -1;
}

static int
hevc_cache_raw_parameter_sets(struct iris_decode_ctx *ctx)
{
	size_t i = 0;
	int found = 0;

	while (i + 3 < ctx->slice_len) {
		int sc = has_start_code(ctx->slice_data + i, ctx->slice_len - i);
		size_t start, end;
		uint8_t type;

		if (!sc) {
			i++;
			continue;
		}
		start = i + sc;
		end = start;
		while (end < ctx->slice_len &&
		       !has_start_code(ctx->slice_data + end,
				       ctx->slice_len - end))
			end++;
		if (end <= start + 1) {
			i = end;
			continue;
		}
		type = (ctx->slice_data[start] >> 1) & 0x3f;
		if (type == 32 && end - start <= sizeof(ctx->raw_hevc_vps)) {
			memcpy(ctx->raw_hevc_vps, ctx->slice_data + start, end - start);
			ctx->raw_hevc_vps_len = end - start;
			found |= 1;
		} else if (type == 33 && end - start <= sizeof(ctx->raw_hevc_sps)) {
			memcpy(ctx->raw_hevc_sps, ctx->slice_data + start, end - start);
			ctx->raw_hevc_sps_len = end - start;
			found |= 2;
		} else if (type == 34 && end - start <= sizeof(ctx->raw_hevc_pps)) {
			memcpy(ctx->raw_hevc_pps, ctx->slice_data + start, end - start);
			ctx->raw_hevc_pps_len = end - start;
			found |= 4;
		}
		i = end;
	}
	return found;
}

int
iris_decode_slice(struct iris_decode_ctx *ctx, const void *data, size_t len)
{
	const uint8_t *p = data;
	size_t need = len + 4;

	if (ctx->out_pixfmt == V4L2_PIX_FMT_AV1) {
		/* AV1 tile payloads are CABAC coded; only the headers are
		 * synthesized, so the bytes are accumulated verbatim. */
		if (ctx->av1.data_len + len > ctx->av1.data_cap) {
			size_t cap = ctx->av1.data_cap ? ctx->av1.data_cap :
				(1u << 20);
			uint8_t *n;

			while (cap < ctx->av1.data_len + len) {
				if (cap > SIZE_MAX / 2)
					return -1;
				cap *= 2;
			}
			n = realloc(ctx->av1.data, cap);
			if (!n)
				return -1;
			ctx->av1.data = n;
			ctx->av1.data_cap = cap;
		}
		ctx->av1.blob_base = (uint32_t)ctx->av1.data_len;
		ctx->av1.blob_ready = 1;
		memcpy(ctx->av1.data + ctx->av1.data_len, p, len);
		ctx->av1.data_len += len;
		return 0;
	}

	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC &&
	    ctx->hevc_slice_next < ctx->hevc_slice_count) {
		unsigned int use = 0;
		unsigned int i;
		size_t data_len = len;
		unsigned int remaining = ctx->hevc_slice_count - ctx->hevc_slice_next;

		/* A single VA slice-data buffer normally contains all ranges.  If a
		 * client uses one data buffer per slice, consume one range at a time. */
		if (remaining > 1) {
			int all_fit = 1;
			for (i = 0; i < remaining; i++) {
				const VASliceParameterBufferHEVC *sp =
					&ctx->hevc_slices[ctx->hevc_slice_next + i].param;

				if ((uint64_t)sp->slice_data_offset +
				    sp->slice_data_size > data_len) {
					all_fit = 0;
					break;
				}
			}
			if (all_fit)
				use = remaining;
		}
		if (!use)
			use = 1;

		for (i = 0; i < use; i++) {
			const VASliceParameterBufferHEVC *sp =
				&ctx->hevc_slices[ctx->hevc_slice_next + i].param;
			uint32_t off = sp->slice_data_offset;
			uint32_t size = sp->slice_data_size;
			uint8_t *rewritten;
			size_t rewritten_cap, nal_len;
			unsigned int pps_id;
			int sc, rewritten_len;
			uint64_t rewrite_start;

			if ((uint64_t)off + size > data_len || !ctx->have_hevc_pic)
				return -1;
			p = (const uint8_t *)data + off;
			sc = has_start_code(p, size);
			nal_len = size - sc;
			if (nal_len > (SIZE_MAX - 1024) / 2)
				return -1;
			rewritten_cap = nal_len * 2 + 1024;
			rewrite_start = ctx->stats_enabled ? monotonic_ns() : 0;
			if (ctx->slice_len > SIZE_MAX - 4 ||
			    rewritten_cap > SIZE_MAX - ctx->slice_len - 4)
				return -1;
			need = 4 + rewritten_cap;
			if (ctx->slice_len + need > ctx->slice_cap) {
				size_t ncap = ctx->slice_cap ? ctx->slice_cap : (1 << 20);

				while (ncap < ctx->slice_len + need) {
					if (ncap > SIZE_MAX / 2)
						return -1;
					ncap *= 2;
				}
				void *n = realloc(ctx->slice_data, ncap);

				if (!n)
					return -1;
				ctx->slice_data = n;
				ctx->slice_cap = ncap;
			}
			rewritten = ctx->slice_data + ctx->slice_len + 4;
			rewritten_len = hevc_rewrite_slice(rewritten, rewritten_cap,
							 p + sc, nal_len,
							 &ctx->hevc_pic, sp,
							 &pps_id);
			if (rewritten_len < 0) {
				fprintf(stderr, "HEVC slice rewrite failed: %d\n",
					rewritten_len);
				return rewritten_len;
			}
			if (ctx->hevc_pps_id < 0)
				ctx->hevc_pps_id = (int)pps_id;
			else if (ctx->hevc_pps_id != (int)pps_id) {
				return -1;
			}
			if ((size_t)rewritten_len != nal_len ||
			    memcmp(rewritten, p + sc, nal_len))
				ctx->hevc_rewritten = 1;
			{
				static const uint8_t sc4[4] = { 0, 0, 0, 1 };
				memcpy(ctx->slice_data + ctx->slice_len, sc4, 4);
				ctx->slice_len += 4;
			}
			ctx->slice_len += rewritten_len;
			if (ctx->stats_enabled) {
				ctx->stats_rewrite_ns += monotonic_ns() - rewrite_start;
				ctx->stats_rewrite_bytes += nal_len;
				ctx->stats_rewrites++;
			}
		}
		ctx->hevc_slice_next += use;
		return 0;
	}

	if (ctx->slice_len + need > ctx->slice_cap) {
		size_t ncap = ctx->slice_cap ? ctx->slice_cap * 2 :
					      (len + (1 << 20));
		void *n = realloc(ctx->slice_data, ncap);

		if (!n)
			return -1;
		ctx->slice_data = n;
		ctx->slice_cap = ncap;
	}

	if (!has_start_code(p, len) && ctx->out_pixfmt != V4L2_PIX_FMT_VP9) {
		static const uint8_t sc4[4] = { 0, 0, 0, 1 };
		memcpy(ctx->slice_data + ctx->slice_len, sc4, 4);
		ctx->slice_len += 4;
	}
	memcpy(ctx->slice_data + ctx->slice_len, p, len);
	ctx->slice_len += len;
	return 0;
}

/* ---- AV1 ----------------------------------------------------------------
 *
 * VA delivers AV1 as parsed picture parameters plus one CABAC payload per
 * tile.  The firmware parses a real OBU stream, so the headers are
 * synthesized (av1_params.c) and the tile payload is passed through.
 *
 * The one piece VA does not carry is refresh_frame_flags.  ref_frame_map[] is
 * the DPB as it stood *before* the picture was decoded (VA clients fill it from
 * their own reference list, which is updated only after the frame is issued),
 * so the refresh set of a picture is recovered from the DPB map of the picture
 * that follows it.  Decoding therefore runs one frame behind: the assembled
 * picture is held in "pending" and submitted when the next one arrives.  A
 * client that syncs the held picture, a flush, a seek or teardown submit it
 * immediately with a best-effort refresh set.
 */

/* Map every reference of @pic onto the slot of our own DPB that holds the same
 * surface.  Which slot the encoder picked stops mattering once the header points
 * at the same picture, and the decoder derives order hints, skip-mode decisions
 * and CDF references from that slot, so the tile data decodes identically.
 * Returns 0 when a wanted surface is no longer in the DPB. */
static int
av1_oh_distance(unsigned int bits, unsigned int a, unsigned int b)
{
	unsigned int m = 1u << bits, d = (a - b) & (m - 1);

	return (int)(d > m / 2 ? m - d : d);
}

static int
av1_hist_order_hint(struct iris_decode_ctx *ctx, VASurfaceID surface)
{
	unsigned int i;

	for (i = 0; i < 64; i++) {
		unsigned int k = (ctx->av1.hist_pos + 64 - 1 - i) % 64;

		if (ctx->av1.hist_surface[k] == surface)
			return ctx->av1.hist_order_hint[k];
	}
	return -1;
}

/* Replace a reference whose surface we no longer hold with the slot of the
 * temporally closest picture we do hold: a missing alternate reference is then
 * predicted from a neighbour a frame or two away instead of from an unrelated
 * picture, which is what turns one eviction into a whole repaired-looking
 * frame rather than a smear.  Without a known order hint, fall back to the most
 * recently decoded picture. */
static unsigned int
av1_substitute_slot(struct iris_decode_ctx *ctx, VASurfaceID want, int used_only)
{
	unsigned int bits = ctx->av1.writer.seq.order_hint_bits;
	unsigned int i, best = AV1_NUM_REF_FRAMES, best_d = 0;
	int want_oh = av1_hist_order_hint(ctx, want);

	for (i = 0; i < AV1_NUM_REF_FRAMES; i++) {
		unsigned int d;

		if (!ctx->av1.writer.slot_valid[i] || !ctx->av1.slot_surface[i])
			continue;
		if (used_only && !ctx->av1.slot_used[i])
			continue;
		d = want_oh >= 0 && bits ?
			(unsigned int)av1_oh_distance(bits, want_oh,
				ctx->av1.writer.slot_order_hint[i]) :
			ctx->av1.au_index - ctx->av1.slot_last_use[i];
		if (best == AV1_NUM_REF_FRAMES || d < best_d) {
			best = i;
			best_d = d;
		}
	}
	return best;
}

/* Map every reference of @pic onto the slot of our own DPB that holds the same
 * surface.  Which slot the encoder picked stops mattering once the header points
 * at the same picture, and the decoder derives order hints, skip-mode decisions
 * and CDF references from that slot, so the tile data decodes identically.
 *
 * A picture that cannot be resolved - its surface was the one evicted when the
 * client's DPB was already full - is pointed at the most recent picture we do
 * have.  Feeding a wrong-but-valid reference costs a slightly wrong frame; the
 * alternative (the encoder's index, which names an unrelated slot) turns the
 * frame and everything that references it into noise, and the firmware rejects
 * a picture whose reference list it cannot satisfy. */
static int
av1_remap_refs(struct iris_decode_ctx *ctx,
	       const VADecPictureParameterBufferAV1 *pic,
	       unsigned int ref_idx[AV1_REFS_PER_FRAME])
{
	VASurfaceID missing[AV1_REFS_PER_FRAME] = { 0 };
	unsigned int i, j;

	memset(ctx->av1.slot_used, 0, sizeof(ctx->av1.slot_used));
	for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
		VASurfaceID want =
			pic->ref_frame_map[pic->ref_frame_idx[i] & 7];

		ref_idx[i] = pic->ref_frame_idx[i] & 7;
		if (want == VA_INVALID_SURFACE || !want)
			continue;
		for (j = 0; j < AV1_NUM_REF_FRAMES; j++)
			if (ctx->av1.writer.slot_valid[j] &&
			    ctx->av1.slot_surface[j] == want) {
				ref_idx[i] = j;
				break;
			}
		if (j == AV1_NUM_REF_FRAMES) {
			missing[i] = want;
			continue;
		}
		ctx->av1.slot_used[j] = 1;
		ctx->av1.slot_last_use[j] = ctx->av1.au_index;

	}

	if (getenv("IRIS_AV1_NO_SUBST"))
		return 0;	/* measurement aid: reproduce the old behaviour */

	for (i = 0; i < AV1_REFS_PER_FRAME; i++) {
		unsigned int sub;

		if (!missing[i])
			continue;
		sub = av1_substitute_slot(ctx, missing[i], 1);
		if (sub == AV1_NUM_REF_FRAMES)
			sub = av1_substitute_slot(ctx, missing[i], 0);
		if (sub == AV1_NUM_REF_FRAMES)
			return 0;
		ref_idx[i] = sub;
		ctx->av1.slot_used[sub] = 1;
		ctx->av1.substituted++;
		if (dbg_enabled())
			fprintf(stderr,
				"[av1-dpb] ref %u: surface %u gone, using slot %u (surface %u)\n",
				i, missing[i], sub, ctx->av1.slot_surface[sub]);
	}
	return 1;
}

/* Choose the DPB slot that receives the new picture.
 *
 * ref_frame_map[] is the client's complete DPB before this picture, so every
 * picture that can still be referenced lives in one of those eight surfaces.
 * A slot is safe to overwrite when its picture is gone from that map, or when the
 * same surface appears twice there (the picture survives in the other slot).
 * Otherwise keep what is used most recently: the encoder drops the reference it
 * needs least, and "not referenced for the longest time" is the closest thing to
 * that which can be observed.  Refreshing a slot the current picture references
 * is not an option - the firmware rejects it even though the spec allows it.
 */
static unsigned int
av1_slots_holding(struct iris_decode_ctx *ctx, unsigned int slot)
{
	unsigned int i, n = 0;

	for (i = 0; i < AV1_NUM_REF_FRAMES; i++)
		if (ctx->av1.writer.slot_valid[i] &&
		    ctx->av1.slot_surface[i] != 0 &&
		    ctx->av1.slot_surface[i] == ctx->av1.slot_surface[slot])
			n++;
	return n;
}

static unsigned int
av1_pick_refresh(struct iris_decode_ctx *ctx,
		 const VADecPictureParameterBufferAV1 *pic)
{
	unsigned int best = AV1_NUM_REF_FRAMES, i, tier;

	for (tier = 0; tier < 3 && best == AV1_NUM_REF_FRAMES; tier++) {
		uint32_t best_use = 0;

		for (i = 0; i < AV1_NUM_REF_FRAMES; i++) {
			unsigned int k, count = 0;

			if (ctx->av1.slot_used[i])
				continue;
			if (!ctx->av1.writer.slot_valid[i]) {
				best = i;
				break;
			}
			/* A slot is safe to overwrite when the client no
			 * longer lists its picture, or when we hold the same
			 * picture in another slot (a key frame fills all of
			 * them).  Counting the client's own duplicates is not
			 * the same thing: two client slots pointing at one of
			 * our pictures means losing both references. */
			for (k = 0; k < AV1_NUM_REF_FRAMES; k++)
				if (pic->ref_frame_map[k] ==
				    ctx->av1.slot_surface[i])
					count++;
			if (tier == 0 && count) {
				continue;
			}
			if (tier == 1 && av1_slots_holding(ctx, i) < 2) {
				continue;
			}
			if (tier == 2) {
				/* Which picture the encoder drops is not visible
				 * here (VA-API carries no refresh_frame_flags), so
				 * this is a measured choice, switchable at runtime
				 * for measurement: IRIS_AV1_VICTIM=rank<N> evicts
				 * the N-th oldest picture, or lru / mru / idx6. */
				const char *rule = getenv("IRIS_AV1_VICTIM");
				uint32_t sc;

				if (rule && !strncmp(rule, "rank", 4)) {
					unsigned int older = 0, j;
					unsigned int want = (unsigned int)atoi(rule + 4);

					for (j = 0; j < AV1_NUM_REF_FRAMES; j++)
						if (j != i &&
						    ctx->av1.slot_stored_at[j] <
						    ctx->av1.slot_stored_at[i])
							older++;
					sc = (older == want);
				} else if (rule && !strcmp(rule, "lru")) {
					sc = 0xffffffffu - ctx->av1.slot_last_use[i];
				} else if (rule && !strcmp(rule, "mru")) {
					sc = ctx->av1.slot_last_use[i];
				} else if (rule && !strcmp(rule, "idx6")) {
					unsigned int k2;

					sc = 0;
					for (k2 = 0; k2 < AV1_NUM_REF_FRAMES; k2++)
						if (k2 == 6 && pic->ref_frame_map[k2] ==
						    ctx->av1.slot_surface[i])
							sc = 1;
				} else {
					/* default: oldest stored */
					sc = 0xffffffffu - ctx->av1.slot_stored_at[i];
				}
				if (best == AV1_NUM_REF_FRAMES || sc > best_use) {
					best = i;
					best_use = sc;
				}
				continue;
			}
			best = i;
			break;
		}
	}
	if (best == AV1_NUM_REF_FRAMES)
		best = 0;
	if (dbg_enabled()) {
		unsigned int cnt = 0, k;

		for (k = 0; k < AV1_NUM_REF_FRAMES; k++)
			if (pic->ref_frame_map[k] == ctx->av1.slot_surface[best])
				cnt++;
		fprintf(stderr,
			"[av1-dpb] pick slot=%u tier=%u surface=%u mapcount=%u used=%u last=%u\n",
			best, tier, ctx->av1.slot_surface[best], cnt,
			(unsigned)ctx->av1.slot_used[best],
			ctx->av1.slot_last_use[best]);
	}
	return 1u << best;
}


/* Per-session AV1 state: our DPB and the synthesized sequence header. */
static void
av1_reset_session(struct iris_decode_ctx *ctx)
{
	memset(ctx->av1.slot_surface, 0, sizeof(ctx->av1.slot_surface));
	memset(ctx->av1.slot_used, 0, sizeof(ctx->av1.slot_used));
	memset(ctx->av1.slot_last_use, 0, sizeof(ctx->av1.slot_last_use));
	memset(ctx->av1.hist_surface, 0, sizeof(ctx->av1.hist_surface));
	ctx->av1.hist_pos = 0;
	ctx->av1.have_order_hint = 0;
	av1_writer_reset(&ctx->av1.writer);
}

/* Drop the picture being assembled.  A seek restart happens in the middle of
 * vaEndPicture and must NOT touch it: the key frame that triggered the restart
 * has already been collected. */
static void
av1_reset_picture(struct iris_decode_ctx *ctx)
{
	ctx->av1.have_pic = 0;
	ctx->av1.n_slices = 0;
	ctx->av1.data_len = 0;
	ctx->av1.blob_ready = 0;
}


/* Best-effort refresh set for a picture whose own refresh set cannot be
 * observed.  Encoders replace the reference they will need least, which is the
 * one with the oldest order hint, so pick that among the slots this picture
 * does not reference.  A wrong choice here costs the firmware a reference (it
 * either errors out or decodes against a stale picture), so staying close to
 * the encoder's usual policy matters.  Refreshing a slot the current picture
 * still references is NOT an option: the Iris firmware errors out on it even
 * though the specification allows it (references are resolved while decoding,
 * the refresh happens afterwards). */
static int
av1_tile_cmp(const void *a, const void *b)
{
	const struct av1_tile *ta = a;
	const struct av1_tile *tb = b;
	unsigned int ka = (unsigned int)ta->row * AV1_MAX_TILES + ta->column;
	unsigned int kb = (unsigned int)tb->row * AV1_MAX_TILES + tb->column;

	return ka < kb ? -1 : ka > kb;
}

static int
av1_record_slice(struct iris_decode_ctx *ctx,
		 const VASliceParameterBufferAV1 *sp)
{
	uint32_t offset = sp->slice_data_offset;

	/* Two client styles reach this point:
	 *  - all tile data first, then one slice parameter per tile whose
	 *    offset already points into the accumulated frame blob
	 *    (Chromium 156, 8 tiles for one 1080p picture), and
	 *  - parameters before the data they describe, where the offset is
	 *    relative to the blob that is about to be appended.
	 * An offset that fits in what has been received so far is absolute;
	 * anything beyond it belongs to the next blob and still needs the
	 * base this driver used to add unconditionally. */
	if ((uint64_t)sp->slice_data_offset + sp->slice_data_size >
	    (uint64_t)ctx->av1.data_len) {
		uint32_t base = ctx->av1.blob_ready ? ctx->av1.blob_base :
			(uint32_t)ctx->av1.data_len;

		offset = base + sp->slice_data_offset;
		DBG("[av1] slice %u offset %u -> %u (base %u, %zu bytes so far)\n",
		    ctx->av1.n_slices, sp->slice_data_offset, offset, base,
		    ctx->av1.data_len);
	}
	ctx->av1.blob_ready = 0;
	if (ctx->av1.n_slices >= AV1_MAX_TILES)
		return -1;
	ctx->av1.slices[ctx->av1.n_slices].offset = offset;
	ctx->av1.slices[ctx->av1.n_slices].size = sp->slice_data_size;
	ctx->av1.slices[ctx->av1.n_slices].row = sp->tile_row;
	ctx->av1.slices[ctx->av1.n_slices].column = sp->tile_column;
	ctx->av1.n_slices++;
	return 0;
}

int
iris_decode_av1_picture(struct iris_decode_ctx *ctx,
			const VADecPictureParameterBufferAV1 *pic)
{
	if (!pic)
		return -EINVAL;
	DBG("[av1] pic-params bitdepth=%u tiles=%ux%u frame=%ux%u\n",
	    pic->bit_depth_idx, pic->tile_cols, pic->tile_rows,
	    pic->frame_width_minus1 + 1, pic->frame_height_minus1 + 1);
	if (pic->bit_depth_idx != 0) {
		/* AV1 Profile 0 covers 8- and 10-bit 4:2:0, so a 10-bit stream
		 * reaches this driver even though only the 8-bit path exists.
		 * Decoding it into NV12 would silently produce corrupt frames;
		 * fail instead so the client falls back to software. */
		if (!ctx->av1.high_bitdepth_warned) {
			fprintf(stderr,
				"iris-vaapi: AV1 %d-bit is not supported (8-bit 4:2:0 only)\n",
				pic->bit_depth_idx == 2 ? 12 : 10);
			ctx->av1.high_bitdepth_warned = 1;
		}
		return -ENOTSUP;
	}
	memcpy(&ctx->av1.pic, pic, sizeof(*pic));
	ctx->av1.have_pic = 1;
	return 0;
}

int
iris_decode_av1_slice_params(struct iris_decode_ctx *ctx,
			     const VASliceParameterBufferAV1 *sp)
{
	if (!sp)
		return -EINVAL;
	return av1_record_slice(ctx, sp);
}

static void
av1_dump(const struct iris_decode_ctx *ctx, const uint8_t *au, size_t len)
{
	const char *path = getenv("IRIS_VAAPI_AV1_DUMP");
	char name[512];
	FILE *f;

	if (!ctx->av1.dump_au || !path || !*path)
		return;
	/* One stream per decode context: clients like FFmpeg spin up a throwaway
	 * context to probe the stream first, and interleaving both dumps would
	 * make the result undecodable. */
	snprintf(name, sizeof(name), "%s.%p", path, (const void *)ctx);
	f = fopen(name, "ab");
	if (!f)
		return;
	fwrite(au, 1, len, f);
	fclose(f);
}

/* Synthesize and queue one AV1 access unit. */
static int
av1_submit(struct iris_decode_ctx *ctx,
	   const VADecPictureParameterBufferAV1 *pic,
	   const struct av1_slice *slices, unsigned int n_slices,
	   const uint8_t *data, size_t data_len,
	   VASurfaceID target, uint64_t generation)
{
	struct av1_tile tiles[AV1_MAX_TILES];
	struct av1_seq_cfg cfg;
	struct iris_surface *qt;
	size_t need, seq_len = 0, frame_len, au_len;
	unsigned int ref_idx[AV1_REFS_PER_FRAME];
	unsigned int refresh_flags, ft;
	uint8_t *au;
	uint64_t ts;
	unsigned int i, count = 0;
	int ret, emit_seq = 0, mapped = 1;

	DBG("[av1] submit n_slices=%u target=%u data=%zu surf=%s\n",
	    n_slices, target, data_len, find_surface(ctx, target) ? "yes" : "no");
	if (!n_slices)
		return -EINVAL;
	if (!find_surface(ctx, target)) {
		DBG("[av1] bail: no surface for target %u\n", target);
		return -EINVAL;
	}
	for (i = 0; i < n_slices; i++) {
		uint64_t off = slices[i].offset;
		uint64_t size = slices[i].size;

		if (off + size > data_len) {
			DBG("[av1] bail: tile %u off=%llu size=%llu > data %zu\n",
			    i, (unsigned long long)off, (unsigned long long)size,
			    data_len);
			return -EINVAL;
		}
		tiles[count].offset = (uint32_t)off;
		tiles[count].size = (uint32_t)size;
		tiles[count].row = slices[i].row;
		tiles[count].column = slices[i].column;
		count++;
	}
	qsort(tiles, count, sizeof(tiles[0]), av1_tile_cmp);

	/* IRIS_AV1_TILE_GROUP=strip handles a client that hands over the raw
	 * tile group (including a tg_start/tg_end pair) instead of the bare
	 * per-tile payloads VA documents.  No known client does. */
	if (getenv("IRIS_AV1_TILE_GROUP") &&
	    strcmp(getenv("IRIS_AV1_TILE_GROUP"), "strip") == 0 &&
	    count == 1 && data_len > 2 && tiles[0].offset == 0 &&
	    tiles[0].size == data_len) {
		tiles[0].offset += 2;
		tiles[0].size -= 2;
	}

	/* May reopen the firmware session (and with it the mirrored DPB). */
	ret = ensure_decoder(ctx);
	if (ret)
		return ret;

	/* Decide the reference mapping and the refresh set from our own DPB. */
	ft = pic->pic_info_fields.bits.frame_type;
	if (ft == AV1_FRAME_KEY && pic->pic_info_fields.bits.show_frame) {
		refresh_flags = 0xff;
		for (i = 0; i < AV1_REFS_PER_FRAME; i++)
			ref_idx[i] = pic->ref_frame_idx[i] & 7;
	} else {
		mapped = av1_remap_refs(ctx, pic, ref_idx);
		refresh_flags = av1_pick_refresh(ctx, pic);
		if (dbg_enabled())
			fprintf(stderr,
				"[av1-dpb] au=%u oh=%u raw=[%u,%u,%u,%u,%u,%u,%u] "
				"map=[%u,%u,%u,%u,%u,%u,%u,%u] "
				"ref=[%u,%u,%u,%u,%u,%u,%u] slots=[%u,%u,%u,%u,%u,%u,%u,%u] "
				"last=[%u,%u,%u,%u,%u,%u,%u,%u] refresh=0x%02x mapped=%d\n",
				ctx->av1.au_index, pic->order_hint,
				pic->ref_frame_idx[0], pic->ref_frame_idx[1],
				pic->ref_frame_idx[2], pic->ref_frame_idx[3],
				pic->ref_frame_idx[4], pic->ref_frame_idx[5],
				pic->ref_frame_idx[6],
				pic->ref_frame_map[0], pic->ref_frame_map[1],
				pic->ref_frame_map[2], pic->ref_frame_map[3],
				pic->ref_frame_map[4], pic->ref_frame_map[5],
				pic->ref_frame_map[6], pic->ref_frame_map[7],
				ref_idx[0], ref_idx[1], ref_idx[2], ref_idx[3],
				ref_idx[4], ref_idx[5], ref_idx[6],
				ctx->av1.slot_surface[0], ctx->av1.slot_surface[1],
				ctx->av1.slot_surface[2], ctx->av1.slot_surface[3],
				ctx->av1.slot_surface[4], ctx->av1.slot_surface[5],
				ctx->av1.slot_surface[6], ctx->av1.slot_surface[7],
				ctx->av1.slot_last_use[0], ctx->av1.slot_last_use[1],
				ctx->av1.slot_last_use[2], ctx->av1.slot_last_use[3],
				ctx->av1.slot_last_use[4], ctx->av1.slot_last_use[5],
				ctx->av1.slot_last_use[6], ctx->av1.slot_last_use[7],
				refresh_flags, mapped);
		if (!mapped)
			DBG("[av1] reference surface gone from DPB\n");
	}

	av1_seq_from_pic(pic, ctx->width, ctx->height, &cfg);
	/* enable_superres, enable_restoration and separate_uv_delta_q are derived
	 * from the picture because VA-API does not carry them, and the pictures
	 * of one stream disagree (a frame with restoration follows one without).
	 * They describe the stream, not the picture: once a frame used one, the
	 * sequence header has to keep advertising it.  Re-emitting a sequence
	 * header mid-stream resets the firmware DPB, which costs every reference
	 * the encoder still relies on. */
	if (ctx->av1.writer.seq_valid) {
		cfg.enable_superres |= ctx->av1.writer.seq.enable_superres;
		cfg.enable_restoration |= ctx->av1.writer.seq.enable_restoration;
		cfg.separate_uv_delta_q |= ctx->av1.writer.seq.separate_uv_delta_q;
	}
	if (dbg_enabled())
		fprintf(stderr,
			"[av1] pic ft=%u show=%u tiles=%ux%u uniform=%u count=%u base_q=%u sct=%u er=%u prf=%u refresh=0x%02x\n",
			pic->pic_info_fields.bits.frame_type,
			pic->pic_info_fields.bits.show_frame,
			pic->tile_cols, pic->tile_rows,
			pic->pic_info_fields.bits.uniform_tile_spacing_flag, count,
			pic->base_qindex,
			pic->pic_info_fields.bits.allow_screen_content_tools,
			pic->pic_info_fields.bits.error_resilient_mode,
			pic->primary_ref_frame, refresh_flags);
	if (!ctx->av1.writer.seq_valid ||
	    !av1_seq_equal(&ctx->av1.writer.seq, &cfg)) {
		DBG("[av1] seq change: old(sr=%d rest=%d uv=%d ohb=%d w=%u h=%u) new(sr=%d rest=%d uv=%d ohb=%d w=%u h=%u)\n",
		    ctx->av1.writer.seq.enable_superres,
		    ctx->av1.writer.seq.enable_restoration,
		    ctx->av1.writer.seq.separate_uv_delta_q,
		    ctx->av1.writer.seq.order_hint_bits,
		    ctx->av1.writer.seq.max_width,
		    ctx->av1.writer.seq.max_height,
		    cfg.enable_superres, cfg.enable_restoration,
		    cfg.separate_uv_delta_q, cfg.order_hint_bits,
		    cfg.max_width, cfg.max_height);
		/* A new sequence header resets the reference buffers. */
		av1_writer_reset(&ctx->av1.writer);
		ctx->av1.writer.seq = cfg;
		ctx->av1.writer.seq_valid = 1;
		emit_seq = 1;
		DBG("[av1] sequence header re-emitted at au=%u\n",
		    ctx->av1.au_index);
	}

	need = 256 + av1_frame_obu_bound(count, tiles);
	if (need > ctx->av1.au_cap) {
		void *n = realloc(ctx->av1.au, need);

		if (!n)
			return -ENOMEM;
		ctx->av1.au = n;
		ctx->av1.au_cap = need;
	}
	au = ctx->av1.au;

	/* OBU_TEMPORAL_DELIMITER: encoders emit one per temporal unit and the
	 * firmware uses it to reset its per-frame state, so keep it. */
	au[0] = (uint8_t)((2 << 3) | 0x02);
	au[1] = 0x00;
	seq_len = 2;
	if (emit_seq) {
		size_t n = av1_write_sequence_header(&cfg, au + seq_len,
						    ctx->av1.au_cap - seq_len);

		if (!n) {
			fprintf(stderr, "iris-vaapi: AV1 sequence header failed\n");
			return -EINVAL;
		}
		seq_len += n;
	}
	frame_len = av1_write_frame_obu(&ctx->av1.writer, pic, tiles, count,
					data, refresh_flags, ref_idx,
					!ctx->av1.no_tile_sizes,
					au + seq_len, ctx->av1.au_cap - seq_len);
	if (!frame_len) {
		fprintf(stderr, "iris-vaapi: AV1 frame OBU failed (%u tiles)\n",
			count);
		return -EINVAL;
	}
	au_len = seq_len + frame_len;
	av1_dump(ctx, au, au_len);

	/* Track our own DPB: the header already points every reference at it. */
	ctx->av1.hist_surface[ctx->av1.hist_pos] =
		pic->current_frame ? pic->current_frame : target;
	ctx->av1.hist_order_hint[ctx->av1.hist_pos] = pic->order_hint;
	ctx->av1.hist_pos = (ctx->av1.hist_pos + 1) % 64;
	for (i = 0; i < AV1_NUM_REF_FRAMES; i++)
		if (refresh_flags & (1u << i)) {
			ctx->av1.slot_surface[i] = pic->current_frame ?
				pic->current_frame : target;
			ctx->av1.slot_last_use[i] = ctx->av1.au_index;
			ctx->av1.slot_stored_at[i] = ctx->av1.au_index;
		}
	ctx->av1.last_order_hint = pic->order_hint;
	ctx->av1.have_order_hint = 1;
	ctx->av1.au_index++;

	/* IRIS_AV1_DUMP_ONLY=1 builds and dumps the access unit but never touches
	 * the VPU.  Header work is then validated against a software decoder
	 * first, which is what catches malformed streams before the firmware
	 * hits its watchdog. */
	if (ctx->av1.dump_only) {
		struct iris_surface *st = find_surface(ctx, target);

		if (st) {
			st->decoded = 1;
			st->queued = 1;
			st->owner = ctx;
		}
		ctx->last_target = target;
		ctx->seq++;
		ctx->last_submit_ns = monotonic_ns();
		return 0;
	}

	ts = (ctx->seq + 1000) * 1000000000ULL;
	if (!ctx->dec_started) {
		/* Mirror the other codecs: queue the first AU before STREAMON. */
		ret = v4l2_dec_feed(&ctx->dec, au, au_len, ts);
		if (ret) {
			finish_pending_writes(ctx);
			return ret;
		}
		ret = v4l2_dec_start(&ctx->dec);
		if (ret) {
			finish_pending_writes(ctx);
			return ret;
		}
		ctx->dec_started = 1;
	} else {
		int spin;

		for (spin = 0; spin < 100; spin++) {
			while (v4l2_dec_dqout(&ctx->dec) == 0)
				;
			ret = v4l2_dec_feed(&ctx->dec, au, au_len, ts);
			ctx->av1.feed_ns = monotonic_ns();
			if (ret != -EAGAIN)
				break;
			ret = drain_available(ctx);
			if (ret)
				break;
			v4l2_dec_poll(&ctx->dec, 50);
		}
		if (ret) {
			finish_pending_writes(ctx);
			return ret;
		}
	}

	ctx->last_target = target;
	qt = find_surface(ctx, target);
	if (qt) {
		uint64_t token = ctx->seq + 1;

		qt->queued = 1;
		qt->owner = ctx;
		if (ctx->direct_capture || (ctx->vk_copy && !ctx->vk_copy_failed))
			surface_begin_device_write(&ctx->dec, qt, token);
		else
			surface_begin_write(&ctx->dec, qt, token);
	}
	{
		unsigned int slot =
			(unsigned int)(ctx->seq % ARRAY_SIZE(ctx->target_ring));

		ctx->target_ring[slot].seq = ctx->seq;
		ctx->target_ring[slot].target = target;
		ctx->target_ring[slot].generation = generation;
		ctx->target_ring[slot].used = 1;
	}
	ctx->seq++;
	ctx->last_submit_ns = monotonic_ns();
	hevc_pending_add(ctx, 0, target, generation);

	return drain_available(ctx);
}

/* Backpressure for exported targets: legacy Adreno may sample a surface before
 * the reservation fence it was handed, so a target the client already imported
 * has to be decoded before vaEndPicture returns.  Returns 1 when it waited, 0
 * when the fast path applied, and a negative error code otherwise. */
static int
iris_wait_end_target(struct iris_decode_ctx *ctx, VASurfaceID target,
		     int wait_exported_codec)
{
	struct iris_surface *s = find_surface(ctx, target);
	int exported_wait = wait_exported_codec && s && s->exported;
	/* AV1 hides most of its frames from the client, so a hidden picture's
	 * surface is released as soon as the decoder is done with it rather
	 * than when the client syncs it; the picture has to be complete before
	 * vaEndPicture returns or the client overwrites it underneath us. */
	int forced_wait = (ctx->out_pixfmt == V4L2_PIX_FMT_H264 &&
			   ctx->force_h264_sync_end) ||
		(ctx->out_pixfmt == V4L2_PIX_FMT_AV1 && !ctx->av1.dump_only &&
		 !getenv("IRIS_AV1_ASYNC"));
	int ret;

	if (!(exported_wait || forced_wait))
		return 0;

	{
		uint64_t wait_start = ctx->stats_enabled ? monotonic_ns() : 0;

		DBG("[end] waiting exported target=%u codec=0x%x\n", target,
		    ctx->out_pixfmt);
		ret = wait_surface_ready(ctx, target, 100);
		if (ctx->stats_enabled && ctx->out_pixfmt == V4L2_PIX_FMT_H264) {
			ctx->stats_h264_wait_ns += monotonic_ns() - wait_start;
			ctx->stats_h264_waits++;
		}
	}
	if (ret && recover_stalled_target(ctx, target) == 0) {
		DBG("[end] target=%u recovered from firmware stall\n", target);
		ret = 0;
	}
	if (ret) {
		struct iris_surface *st = find_surface(ctx, target);

		/* Last resort: serve the surface's previous contents for this
		 * one frame rather than failing the stream. */
		if (!ctx->fatal_error) {
			fprintf(stderr,
				"[end] target=%u not decoded after stall recovery; serving stale frame\n",
				target);
			if (st)
				surface_finish_write(&ctx->dec, st);
			if (st) {
				st->decoded = 1;
				st->queued = 1;
			}
			ret = 0;
		} else {
			DBG("[end] target=%u readiness wait failed: %d\n",
			    target, ret);
			return ret;
		}
	}
	DBG("[end] exported target=%u ready\n", target);
	return 1;
}

static void
av1_skipped(struct iris_decode_ctx *ctx)
{
	ctx->av1.skips++;
	if (ctx->stats_enabled)
		fprintf(stderr, "[iris-av1] picture skipped (%llu so far)\n",
			(unsigned long long)ctx->av1.skips);
}

static int
iris_decode_av1_end(struct iris_decode_ctx *ctx)
{
	uint64_t end_start = ctx->stats_enabled ? monotonic_ns() : 0;
	int random_access, ret;

	DBG("[av1] end have_pic=%d slices=%u data=%zu target=%u dec_open=%d\n",
	    ctx->av1.have_pic, ctx->av1.n_slices, ctx->av1.data_len,
	    ctx->current_target, ctx->dec_open);
	if (!ctx->av1.have_pic)
		return -1;

	random_access = is_random_access_picture(ctx);
	if (ctx->dec_started && random_access) {
		unsigned int oh_bits = ctx->av1.writer.seq.order_hint_bits;
		int seek_boundary;

		/* Chromium does not forward Decoder::Reset() over libva, so a seek
		 * is only visible as a random-access picture.  The pause-based
		 * heuristic used by the other codecs misfires on slow 4K streams
		 * (decoding a normal frame can take longer than the threshold).
		 * AV1 carries order hints, and a real seek - or a loop back to the
		 * start - makes the hint go backwards, so use that instead. */
		if (oh_bits && ctx->av1.have_order_hint) {
			int m = 1 << oh_bits;
			int diff = (int)ctx->av1.pic.order_hint -
				(int)ctx->av1.last_order_hint;

			diff = ((diff % m) + m) % m;
			seek_boundary = ctx->eos_sent || diff > m / 2;
		} else {
			uint64_t gap = ctx->last_submit_ns ?
				monotonic_ns() - ctx->last_submit_ns : 0;

			seek_boundary = ctx->eos_sent ||
				gap >= IRIS_SEEK_GAP_NS;
		}

		if (seek_boundary) {
			DBG("[av1] seek boundary (order hint %u -> %u): restarting session\n",
			    ctx->av1.last_order_hint, ctx->av1.pic.order_hint);
			ret = drain_and_restart_seek(ctx);
			if (ret) {
				/* Never abort the client because a restart failed;
				 * drop the session so the next picture reopens it. */
				fprintf(stderr,
					"iris-vaapi: AV1 seek restart failed (%d); reopening\n",
					ret);
				iris_decode_reset(ctx);
				ctx->av1.have_order_hint = 0;
			}
		}
	}

	ret = ensure_decoder(ctx);
	DBG("[av1] ensure_decoder -> %d (dec_open=%d)\n", ret, ctx->dec_open);
	if (ret)
		return ret;

	ret = av1_submit(ctx, &ctx->av1.pic, ctx->av1.slices,
			 ctx->av1.n_slices, ctx->av1.data, ctx->av1.data_len,
			 ctx->current_target, ctx->current_generation);
	if (ret == 1) {
		struct iris_surface *sk = find_surface(ctx, ctx->current_target);

		/* Skipped: serve the surface's previous contents so a lost
		 * reference costs one frame instead of the whole stream. */
		if (sk) {
			surface_finish_write(&ctx->dec, sk);
			sk->decoded = 1;
			sk->queued = 1;
			sk->owner = ctx;
		}
		av1_skipped(ctx);
	} else if (ret) {
		return ret;
	} else {
		ret = iris_wait_end_target(ctx, ctx->current_target, 1);
		if (ret < 0)
			return ret;
	}

	ret = iris_wait_end_target(ctx, ctx->current_target, 1);
	if (ret < 0)
		return ret;

	ctx->av1.have_pic = 0;
	ctx->av1.n_slices = 0;
	ctx->av1.data_len = 0;
	ctx->av1.blob_ready = 0;
	if (ctx->stats_enabled) {
		ctx->stats_end_ns += monotonic_ns() - end_start;
		ctx->stats_ends++;
	}
	return 0;
}

static int
iris_decode_end_impl(struct iris_decode_ctx *ctx)
{
	uint8_t *au;
	int rv;
	int random_access;
	int vp9_barrier_submitted = 0;
	uint64_t end_start = ctx->stats_enabled ? monotonic_ns() : 0;

	if (ctx->out_pixfmt == V4L2_PIX_FMT_AV1)
		return iris_decode_av1_end(ctx);

	DBG("[end] target=%u slice_len=%zu refs=%d/%d started=%d\n",
	    ctx->current_target, ctx->slice_len, ctx->refs_l0, ctx->refs_l1,
	    ctx->dec_started);
	size_t au_len = 0, au_cap;
	int n, ret;
	static const uint8_t sc4[4] = { 0, 0, 0, 1 };

	if (!ctx->have_pic && ctx->out_pixfmt != V4L2_PIX_FMT_VP9 &&
	    !ctx->have_hevc_pic)
		return -1;
	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC &&
	    ctx->hevc_slice_next != ctx->hevc_slice_count)
		return -1;
	random_access = is_random_access_picture(ctx);

	/* Stateful firmware may retain pictures from before a Chromium seek because
	 * decoder Reset() has no libva counterpart.  At the first safe random-access
	 * picture, drain every codec's old session through LAST before reopening it,
	 * so old and new access units never coexist in VPU.  Keep the driver's private
	 * timestamp sequence monotonic across the restart. */
	if (ctx->dec_started && random_access) {
		uint64_t now = monotonic_ns();
		uint64_t gap = ctx->last_submit_ns ? now - ctx->last_submit_ns : 0;
		int seek_boundary = ctx->eos_sent || gap >= IRIS_SEEK_GAP_NS;

		/* Do not restart on normal in-stream IDRs: some content has a key
		 * frame every few hundred milliseconds.  EOS is unambiguous; without
		 * EOS, the pause before the random-access picture distinguishes a seek
		 * from normal frame cadence. */
		if (seek_boundary) {
			DBG("[end] seek boundary gap=%llums codec=0x%x: draining old session\n",
			    (unsigned long long)(gap / 1000000ULL),
			    ctx->out_pixfmt);
			ret = drain_and_restart_seek(ctx);
			if (ret)
				return ret;
		}
	}

	/* 16 MiB of stack would overflow the caller's stack; use the heap. */
	au_cap = ctx->slice_len + sizeof(ctx->last_sps) +
		 sizeof(ctx->last_pps) + 16;
	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC)
		au_cap += sizeof(ctx->raw_hevc_vps) + sizeof(ctx->raw_hevc_sps) +
			  sizeof(ctx->raw_hevc_pps) + sizeof(ctx->last_hevc_sps) +
			  16;
	if (ctx->au_cap < au_cap) {
		void *n = realloc(ctx->au_data, au_cap);

		if (!n)
			return -1;
		ctx->au_data = n;
		ctx->au_cap = au_cap;
	}
	au = ctx->au_data;
	ret = ensure_decoder(ctx);
	if (ret)
		return ret;

	/* Assemble the access unit to feed the stateful firmware.
	 *
	 * H.264: VA clients send picture/slice parameter buffers, so re-serialize
	 * SPS/PPS.  HEVC prefers original VPS/SPS/PPS NALs when available and
	 * only uses the serializer as a compatibility fallback.
	 *
	 * VP9: each frame is self-contained (its own uncompressed+compressed
	 * header); the slice data is the whole frame, feed it verbatim. */
	if (ctx->out_pixfmt == V4L2_PIX_FMT_VP9) {
		au_len = ctx->slice_len;
		memcpy(au, ctx->slice_data, au_len);
	} else if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC) {
		int raw_in_au = ctx->hevc_rewritten ? 0 :
			hevc_cache_raw_parameter_sets(ctx);
		/* If the client provided complete parameter NALs, pass the original
		 * Annex-B access unit through unchanged.  This is the native input
		 * contract of the stateful Iris V4L2 decoder. */
		if (raw_in_au == 7 && !ctx->hevc_rewritten) {
			au_len = ctx->slice_len;
			memcpy(au, ctx->slice_data, au_len);
			goto hevc_au_ready;
		}
		if (!ctx->hevc_rewritten && ctx->raw_hevc_vps_len &&
		    ctx->raw_hevc_sps_len && ctx->raw_hevc_pps_len) {
			memcpy(au + au_len, sc4, 4);
			memcpy(au + au_len + 4, ctx->raw_hevc_vps,
			       ctx->raw_hevc_vps_len);
			au_len += 4 + ctx->raw_hevc_vps_len;
			memcpy(au + au_len, sc4, 4);
			memcpy(au + au_len + 4, ctx->raw_hevc_sps,
			       ctx->raw_hevc_sps_len);
			au_len += 4 + ctx->raw_hevc_sps_len;
			memcpy(au + au_len, sc4, 4);
			memcpy(au + au_len + 4, ctx->raw_hevc_pps,
			       ctx->raw_hevc_pps_len);
			au_len += 4 + ctx->raw_hevc_pps_len;
			memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
			au_len += ctx->slice_len;
			goto hevc_au_ready;
		}
		/* Re-serialize VPS/SPS/PPS from the picture params and prepend
		 * only when they change (per-picture repetition resets the
		 * firmware DPB).  ffmpeg/Chrome send bare slice NALs. */
		n = hevc_build_vps(au + 4, au_cap - 4 - ctx->slice_len,
				   &ctx->hevc_pic);
		if (n <= 0) {
			return -1;
		}
		if (n > 0 && (n != ctx->last_hevc_vps_len ||
			      memcmp(au + 4, ctx->last_hevc_vps, n) != 0)) {
			memcpy(au, sc4, 4);
			memcpy(ctx->last_hevc_vps, au + 4, n);
			ctx->last_hevc_vps_len = n;
			au_len = 4 + n;
		}
		n = hevc_build_sps(au + au_len + 4, au_cap - au_len - 4 -
				   ctx->slice_len, &ctx->hevc_pic,
				   ctx->have_hevc_iq ? &ctx->hevc_iq : NULL);
		if (n <= 0) {
			return -1;
		}
		if (n > 0 && (n != ctx->last_hevc_sps_len ||
			      memcmp(au + au_len + 4, ctx->last_hevc_sps, n) != 0)) {
			memcpy(au + au_len, sc4, 4);
			memcpy(ctx->last_hevc_sps, au + au_len + 4, n);
			ctx->last_hevc_sps_len = n;
			au_len += 4 + n;
		}
		n = hevc_build_pps_id(au + au_len + 4, au_cap - au_len - 4 -
				      ctx->slice_len, &ctx->hevc_pic,
				      ctx->hevc_pps_id < 0 ? 0 :
				      (unsigned int)ctx->hevc_pps_id);
		if (n <= 0) {
			return -1;
		}
		if (n > 0 && (n != ctx->last_hevc_pps_len ||
			      memcmp(au + au_len + 4, ctx->last_hevc_pps, n) != 0)) {
			memcpy(au + au_len, sc4, 4);
			memcpy(ctx->last_hevc_pps, au + au_len + 4, n);
			ctx->last_hevc_pps_len = n;
			au_len += 4 + n;
		}
		if (au_len + ctx->slice_len > au_cap) {
			return -1;
		}
		memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
		au_len += ctx->slice_len;
	hevc_au_ready:
		;
	} else {
	/* An IDR slice does not use the PPS reference defaults, so VA reports
	 * zero for both lists.  Starting the stream with those zeros and later
	 * redefining pps_id 0 when the first P/B slice arrives crashes legacy
	 * Venus firmware.  Seed the conventional decoder default (up to three
	 * active L0 references, bounded by max_num_ref_frames); a later slice
	 * that actually uses the defaults supplies the exact values. */
	if (ctx->h264_pps_refs_l0 < 0)
		ctx->h264_pps_refs_l0 = ctx->pic.num_ref_frames ?
			(ctx->pic.num_ref_frames > 3 ? 2 :
			 ctx->pic.num_ref_frames - 1) : 0;
	if (ctx->h264_pps_refs_l1 < 0)
		ctx->h264_pps_refs_l1 = 0;
	if (h264_update_pps_defaults(ctx))
		return -1;
	/* Only re-emit SPS/PPS when they change; a per-picture repetition
	 * resets the firmware DPB and breaks P-frame references. */
	n = h264_build_sps(au + 4, au_cap - 4 - ctx->slice_len, &ctx->pic,
			   ctx->profile, ctx->width, ctx->height);
	if (n <= 0) {
		return -1;
	}
	if (n != ctx->last_sps_len ||
	    memcmp(au + 4, ctx->last_sps, n) != 0) {
		memcpy(au, sc4, 4);
		memcpy(ctx->last_sps, au + 4, n);
		ctx->last_sps_len = n;
		au_len = 4 + n;
	}

	n = h264_build_pps(au + au_len + 4, au_cap - au_len - 4 -
			   ctx->slice_len, &ctx->pic,
			   ctx->have_h264_iq ? &ctx->h264_iq : NULL,
			   ctx->h264_pps_refs_l0 < 0 ? 0 :
			   ctx->h264_pps_refs_l0,
			   ctx->h264_pps_refs_l1 < 0 ? 0 :
			   ctx->h264_pps_refs_l1);
	if (n <= 0) {
		return -1;
	}
	if (n != ctx->last_pps_len ||
	    memcmp(au + au_len + 4, ctx->last_pps, n) != 0) {
		memcpy(au + au_len, sc4, 4);
		memcpy(ctx->last_pps, au + au_len + 4, n);
		ctx->last_pps_len = n;
		au_len += 4 + n;
	}

	if (au_len + ctx->slice_len > au_cap) {
		return -1;
	}
	memcpy(au + au_len, ctx->slice_data, ctx->slice_len);
	au_len += ctx->slice_len;
	}

	/* Each V4L2 OUTPUT buffer must contain exactly one access unit.  In
	 * particular, do not append an AUD: it starts an empty next access unit
	 * and crashes legacy Iris firmware on otherwise valid streams. */

	/* Opt-in bitstream capture for validating the VA-to-stateful translation
	 * with an independent software decoder. */
	if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC) {
		const char *dump_path = getenv("IRIS_HEVC_DUMP");

		if (dump_path && *dump_path) {
			FILE *dump = fopen(dump_path, "ab");

			if (dump) {
				fwrite(au, 1, au_len, dump);
				fclose(dump);
			}
		}
	} else if (ctx->out_pixfmt == V4L2_PIX_FMT_H264) {
		const char *dump_path = getenv("IRIS_H264_DUMP");

		if (dump_path && *dump_path) {
			FILE *dump = fopen(dump_path, "ab");

			if (dump) {
				fwrite(au, 1, au_len, dump);
				fclose(dump);
			}
		}
	}

	{
		uint64_t ts = (ctx->seq + 1000) * 1000000000ULL;

		ctx->last_target = ctx->current_target;
		{
			struct iris_surface *qt = find_surface(ctx,
							       ctx->current_target);

			if (qt) {
				uint64_t token = ctx->seq + 1;

				qt->queued = 1;
				qt->owner = ctx;
				if (ctx->direct_capture ||
				    (ctx->vk_copy && !ctx->vk_copy_failed))
					surface_begin_device_write(&ctx->dec, qt, token);
				else
					surface_begin_write(&ctx->dec, qt, token);
			}
		}
		/* Ring mapping: only a handful of frames (bounded by the
		 * CAPTURE buffer count) are ever in flight, so a slot is
		 * reused long after its previous frame was dequeued.  seq
		 * itself keeps growing so timestamps stay unique. */
		{
			unsigned int slot = (unsigned int)(ctx->seq %
				ARRAY_SIZE(ctx->target_ring));

			ctx->target_ring[slot].seq = ctx->seq;
			ctx->target_ring[slot].target = ctx->current_target;
			ctx->target_ring[slot].generation =
				ctx->current_generation;
			ctx->target_ring[slot].used = 1;
		}

		if (!ctx->dec_started) {
			/* Mirror FFmpeg: queue the first access unit before
			 * STREAMON, otherwise the firmware rejects the CAPTURE
			 * setup (0x1004). */
			ret = v4l2_dec_feed(&ctx->dec, au, au_len, ts);
			if (ret) {
				finish_pending_writes(ctx);
				return ret;
			}
			ret = v4l2_dec_start(&ctx->dec);
			if (ret) {
				finish_pending_writes(ctx);
				return ret;
			}
			ctx->dec_started = 1;
		} else {
			/* Wait for a free OUTPUT buffer, then queue.  The firmware
			 * stops consuming input when its CAPTURE queue fills, so
			 * drain finished frames while we wait or we deadlock. */
			for (int spin = 0; spin < 100; spin++) {
				while (v4l2_dec_dqout(&ctx->dec) == 0)
					;
				ret = v4l2_dec_feed(&ctx->dec, au, au_len, ts);
				if (ret != -EAGAIN)
					break;
				ret = drain_available(ctx);
				if (ret)
					break;
				v4l2_dec_poll(&ctx->dec, 50);
			}
			if (ret) {
				finish_pending_writes(ctx);
				return ret;
			}
		}
		if (ctx->out_pixfmt == V4L2_PIX_FMT_HEVC)
			hevc_pending_add(ctx,
					 ctx->hevc_pic.CurrPic.pic_order_cnt,
					 ctx->current_target,
					 ctx->current_generation);
		ctx->seq++;
	}
	/* Legacy VPU5 retains the current VP9 picture until another access unit
	 * arrives.  Chrome may present an exported key-frame target immediately
	 * after EndPicture and does not reliably observe a reservation fence added
	 * after the DMA-BUF was imported.  Queue one internal duplicate key frame
	 * to release the real target, then wait briefly for that real completion.
	 * The duplicate has its own timestamp but no target-ring mapping, so its
	 * eventual CAPTURE output is recycled rather than exposed to Chrome. */
	if (ctx->out_pixfmt == V4L2_PIX_FMT_VP9 && random_access) {
		uint64_t prime_ts = (ctx->seq + 1000) * 1000000000ULL;

		for (int spin = 0; spin < 100; spin++) {
			while (v4l2_dec_dqout(&ctx->dec) == 0)
				;
			ret = v4l2_dec_feed(&ctx->dec, au, au_len, prime_ts);
			if (ret != -EAGAIN)
				break;
			ret = drain_available(ctx);
			if (ret)
				break;
			v4l2_dec_poll(&ctx->dec, 50);
		}
		if (ret)
			return ret;
		DBG("[vp9-keyframe] prime seq=%llu target=%u\n",
		    (unsigned long long)ctx->seq, ctx->current_target);
		ctx->seq++;
	}
	/* The seek key frame above is complete before EndPicture returns, but VPU5
	 * would hold the immediately following real inter frame.  ANGLE can sample
	 * its exported target before the next Chrome decode call and briefly expose
	 * the target's pre-seek pixels.  One show_existing_frame AU pushes that first
	 * inter frame out without decoding a duplicate or changing VP9 references. */
	if (ctx->out_pixfmt == V4L2_PIX_FMT_VP9 &&
	    ctx->vp9_seek_barrier && !random_access) {
		uint8_t show_existing[2];
		size_t show_existing_len = vp9_show_existing_au(ctx, show_existing);
		uint64_t barrier_ts = (ctx->seq + 1000) * 1000000000ULL;

		if (!show_existing_len)
			return -EINVAL;
		for (int spin = 0; spin < 100; spin++) {
			while (v4l2_dec_dqout(&ctx->dec) == 0)
				;
			ret = v4l2_dec_feed(&ctx->dec, show_existing,
					    show_existing_len, barrier_ts);
			if (ret != -EAGAIN)
				break;
			ret = drain_available(ctx);
			if (ret)
				break;
			v4l2_dec_poll(&ctx->dec, 50);
		}
		if (ret)
			return ret;
		DBG("[vp9-seek] inter barrier seq=%llu target=%u\n",
		    (unsigned long long)ctx->seq, ctx->current_target);
		ctx->seq++;
		ctx->vp9_seek_barrier = 0;
		vp9_barrier_submitted = 1;
	}
	/* The stateful firmware holds each frame until the next access unit
	 * arrives.  Draining right after this feed makes the *previous*
	 * picture's frame available so a client that syncs one picture at a
	 * time (ffmpeg/Chrome) does not deadlock. */
	ret = drain_available(ctx);
	if (ret)
		return ret;
	if (ctx->out_pixfmt == V4L2_PIX_FMT_VP9 &&
	    (random_access || vp9_barrier_submitted)) {
		ret = wait_surface_ready(ctx, ctx->current_target, 20);
		if (ret) {
			DBG("[vp9-barrier] target=%u wait incomplete: %d\n",
			    ctx->current_target, ret);
			/* A seek boundary is specifically protecting ANGLE from stale
			 * pixels.  Prefer decoder fallback to returning that surface
			 * before it is ready. */
			if (ctx->vp9_seek_barrier || vp9_barrier_submitted)
				return ret;
		}
	}

	/* Chrome sends an already-exported surface to ANGLE immediately after
	 * vaEndPicture.  Legacy Adreno can sample its previous contents even though
	 * a new reservation fence was attached.  Asynchronous HEVC let Chrome
	 * recycle a target one or two generations before its CAPTURE frame arrived,
	 * so H.264 and HEVC exported targets need backpressure here.  VP9 normally
	 * stays asynchronous because its firmware holds the current picture until
	 * the next frame; its safe key-frame and post-seek barriers are handled
	 * above only after an internal AU has pushed the target out. */
	{
		int wait_exported_codec = ctx->out_pixfmt == V4L2_PIX_FMT_H264 ||
			ctx->out_pixfmt == V4L2_PIX_FMT_HEVC;
		int waited;

		waited = iris_wait_end_target(ctx, ctx->current_target,
					      wait_exported_codec);
		if (waited < 0)
			return waited;
		if (ctx->stats_enabled && ctx->out_pixfmt == V4L2_PIX_FMT_H264 &&
		    !waited)
			ctx->stats_h264_async++;
	}
	ctx->slice_len = 0;
	ctx->have_pic = 0;
	rv = 0;
	ctx->last_submit_ns = monotonic_ns();
	if (ctx->stats_enabled) {
		ctx->stats_end_ns += monotonic_ns() - end_start;
		ctx->stats_ends++;
	}
	DBG("[end] done rv=%d\n", rv);
	return rv;
}

int
iris_decode_end(struct iris_decode_ctx *ctx)
{
	int ret;

	pthread_mutex_lock(&ctx->mutex);
	ret = iris_decode_end_impl(ctx);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

static int
iris_decode_sync_impl(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	struct iris_surface *s = find_surface(ctx, id);
	int deadline = 200;	/* ~2 s */

	/* Chrome preallocates a surface pool and syncs each freshly created
	 * (never-decoded) surface before exporting it.  The backing buffer is
	 * already valid, so an unqueued surface syncs immediately. */
	if (!s)
		return -EINVAL;
	if (ctx->fatal_error)
		return ctx->fatal_error;
	/* AV1 keeps the newest picture until the next one arrives; a client that
	 * syncs it is asking for it now, so resolve it with a best-effort
	 * refresh set before the "never queued" shortcut below. */
	if (!s->queued)
		return 0;

	if (iris_decode_surface_ready(ctx, id))
		return 0;
	if (ctx->fatal_error)
		return ctx->fatal_error;

	/* Do NOT flush eagerly when the client syncs the most recently queued
	 * target: synchronous clients (Firefox) sync right after submitting,
	 * before the firmware has processed the AU, and a mid-stream EOS kick
	 * (V4L2_DEC_CMD_STOP) tears down the session. Decode-order output
	 * makes the target appear without needing another AU, so plain
	 * waiting is sufficient; the flush below is only a last resort after
	 * the wait really times out (genuine firmware hold / end of stream). */
	while (deadline-- > 0) {
		struct v4l2_dec_frame frame;
		int changed, ret;

		/* Wait for real CAPTURE progress; POLLOUT would wake us
		 * instantly and burn the deadline before any frame is done. */
		ret = v4l2_dec_poll_cap(&ctx->dec, 20);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (!ret)
			continue;
		(void)changed;
		ret = v4l2_dec_handle_events(&ctx->dec, &changed);
		if (ret)
			return ret;
		while (v4l2_dec_dqout(&ctx->dec) == 0)
			;
		ret = v4l2_dec_dqcap(&ctx->dec, &frame);
		if (ret == -EAGAIN)
			continue;
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		DBG("[sync] got ts=%llu\n",
		    (unsigned long long)frame.timestamp);
		ret = assign_frame(ctx, &frame);
		if (ret < 0) {
			ctx->fatal_error = ret;
			return ret;
		}
		if (iris_decode_surface_ready(ctx, id))
			return 0;
	}

	/* Last resort after a genuine wait timeout: force the held target out
	 * via EOS. The driver's reopen logic recovers the session afterwards. */
	if (id == ctx->last_target && !ctx->eos_sent) {
		DBG("[sync] timed out, flushing last_target=%u\n", id);
		if (iris_decode_flush(ctx) == 0 &&
		    iris_decode_surface_ready(ctx, id))
			return 0;
	}
	return -ETIMEDOUT;
}

int
iris_decode_sync(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	int ret;

	pthread_mutex_lock(&ctx->mutex);
	ret = iris_decode_sync_impl(ctx, id);
	pthread_mutex_unlock(&ctx->mutex);
	return ret;
}

static int
iris_decode_surface_ready(struct iris_decode_ctx *ctx, VASurfaceID id)
{
	if (drain_available(ctx))
		return 0;
	if (finish_vk_surface(ctx, id, 0) < 0)
		return 0;
	{
		struct iris_surface *s = find_surface(ctx, id);

		return s ? s->decoded : 0;
	}
}
