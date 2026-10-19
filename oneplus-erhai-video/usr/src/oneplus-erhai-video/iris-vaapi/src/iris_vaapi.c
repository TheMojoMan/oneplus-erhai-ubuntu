// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * iris-vaapi: VA-API driver for the Qualcomm Iris (SM8150) stateful V4L2
 * decoder.
 */

#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include <va/va.h>
#include <va/va_backend.h>
#include <va/va_drmcommon.h>

#ifndef ALIGN
#define ALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#endif
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef DRM_FORMAT_NV12
#define DRM_FORMAT_NV12	0x3231564e	/* NV12 fourcc */
#endif
#ifndef DRM_FORMAT_P010
#define DRM_FORMAT_P010	0x30313050	/* P010 fourcc */
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR	0
#endif
#ifndef DRM_FORMAT_R8
#define DRM_FORMAT_R8		0x20203852	/* 'R' '8' 0x20 */
#endif
#ifndef DRM_FORMAT_GR88
#define DRM_FORMAT_GR88		0x38385247	/* 'GR88' */
#endif
#ifndef DRM_FORMAT_R16
#define DRM_FORMAT_R16		0x20363152	/* 'R16 ' */
#endif
#ifndef DRM_FORMAT_GR1616
#define DRM_FORMAT_GR1616	0x32335247	/* 'GR32' */
#endif

#include "decode.h" 
#include "v4l2_dec.h"

#define IRIS_VAAPI_VERSION	"0.1.0"
#define IRIS_VAAPI_VENDOR	"iris-vaapi: Qualcomm Iris SM8150 (V4L2) "

/* Per-surface tracing is chatty and the GPU process inherits this stderr;
 * opt in with IRIS_VAAPI_DEBUG=1. */
static int dbg_enabled(void)
{
	static int dbg = -1;

	if (dbg < 0)
		dbg = getenv("IRIS_VAAPI_DEBUG") != NULL;
	return dbg;
}

#define DBG(...)	do { if (dbg_enabled()) fprintf(stderr, __VA_ARGS__); } while (0)

/* vtable needs a little state to hand out ids */
#define IRIS_MAX_ENGINES	8
#define IRIS_MAX_CONFIGS	32

struct iris_config {
	VAConfigID id;
	VAProfile profile;
};

struct iris_engine {
	VAContextID ctx_id;
	VAProfile profile;
	struct iris_decode_ctx *dec;
};

struct iris_drv_data {
	unsigned int config_id;
	unsigned int context_id;
	unsigned int surface_id;
	unsigned int buffer_id;
	unsigned int width, height;
	int p010_supported;
	struct iris_config configs[IRIS_MAX_CONFIGS];
	int n_configs;
	/* Display-level surface registry: pool surfaces outlive the contexts
	 * that decode into them (Chrome destroys contexts on navigation while
	 * frames are still exported/displayed). */
	struct iris_surfs *surfs;
	/* One engine (one V4L2 session) per VA context so concurrent videos
	 * never share firmware DPB/queue state. */
	struct iris_engine engines[IRIS_MAX_ENGINES];
	int n_engines;
	int n_bufs;
	unsigned int buf_ids[256];
	VABufferType buf_types[256];
	unsigned int buf_sizes[256];
	unsigned int buf_num_elements[256];
	void *buf_data[256];
	VASurfaceID direct_spares[32];
	unsigned int direct_spare_count;
	unsigned int direct_spare_width;
	unsigned int direct_spare_height;
	unsigned int direct_spare_fourcc;
	int direct_pool_initialized;

	/* Derived images (vaDeriveImage) map a VABufferID to a CAPTURE buffer. */
	VABufferID derived_ids[256];
	void *derived_mem[256];
	int derived_n;

	/* vaCreateImage/vaGetImage CPU transfer buffers. */
	VABufferID img_ids[64];
	unsigned int img_sizes[64];
	unsigned int img_ws[64];
	unsigned int img_hs[64];
	unsigned int img_fourcc[64];
	void *img_data[64];
	int img_n;
};

static const VAProfile iris_profiles[] = {
	VAProfileH264ConstrainedBaseline,
	VAProfileH264Main,
	VAProfileH264High,
	VAProfileHEVCMain,
	VAProfileHEVCMain10,
	VAProfileVP9Profile0,
	VAProfileVP9Profile2,
};
#define NUM_IRIS_PROFILES (sizeof(iris_profiles) / sizeof(iris_profiles[0]))

static const VAEntrypoint iris_entrypoints[] = {
	VAEntrypointVLD,
};
#define NUM_IRIS_ENTRYPOINTS (sizeof(iris_entrypoints) / sizeof(iris_entrypoints[0]))

static int
iris_profile_is_10bit(VAProfile profile)
{
	return profile == VAProfileHEVCMain10 ||
	       profile == VAProfileVP9Profile2;
}

static unsigned int
iris_profile_fourcc(VAProfile profile)
{
	return iris_profile_is_10bit(profile) ? VA_FOURCC_P010 : VA_FOURCC_NV12;
}

static unsigned int
iris_profile_rt_format(VAProfile profile)
{
	/* 10-bit profiles also expose the plain YUV420 RT format: Chromium's
	 * vaapi_wrapper requires VA_RT_FORMAT_YUV420 for VAProfileHEVCMain10
	 * (only VP9Profile2/3 ask for YUV420_10BPP) and drops the profile from
	 * its supported list when the check fails. */
	return iris_profile_is_10bit(profile) ?
		(VA_RT_FORMAT_YUV420 | VA_RT_FORMAT_YUV420_10) :
		VA_RT_FORMAT_YUV420;
}

static unsigned int
iris_surface_pitch(unsigned int width, unsigned int fourcc)
{
	unsigned int bytes = fourcc == VA_FOURCC_P010 ? 2 : 1;
	unsigned int alignment = fourcc == VA_FOURCC_P010 ? 256 : 128;

	return ALIGN(width * bytes, alignment);
}

static int
iris_profile_supported(VAProfile profile)
{
	unsigned int i;

	for (i = 0; i < NUM_IRIS_PROFILES; i++)
		if (iris_profiles[i] == profile)
			return 1;
	return 0;
}

static struct iris_config *
iris_find_config(struct iris_drv_data *dd, VAConfigID id)
{
	int i;

	for (i = 0; i < dd->n_configs; i++)
		if (dd->configs[i].id == id)
			return &dd->configs[i];
	return NULL;
}

static struct iris_drv_data *
iris_drv_data(VADriverContextP ctx)
{
	if (!ctx)
		return NULL;
	if (!ctx->pDriverData) {
		ctx->pDriverData = calloc(1, sizeof(struct iris_drv_data));
		if (!ctx->pDriverData)
			return NULL;
	}
	return ctx->pDriverData;
}

static void
iris_free_buffers(struct iris_drv_data *dd)
{
	int i;

	for (i = 0; i < dd->n_bufs; i++)
		free(dd->buf_data[i]);
	dd->n_bufs = 0;
}

static VAStatus
iris_vaTerminate(VADriverContextP ctx)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	int i;

	if (dd) {
		iris_free_buffers(dd);
		for (i = 0; i < dd->img_n; i++)
			free(dd->img_data[i]);
		dd->img_n = 0;
		dd->derived_n = 0;
		for (i = 0; i < dd->n_engines; i++)
			iris_decode_destroy(dd->engines[i].dec);
		dd->n_engines = 0;
		iris_surfs_destroy(dd->surfs);
		dd->surfs = NULL;
		free(dd);
		ctx->pDriverData = NULL;
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryConfigProfiles(VADriverContextP ctx, VAProfile *profile_list,
			   int *num_profiles)
{
	struct iris_drv_data *dd;
	int count = 0;
	unsigned int i;

	if (!num_profiles)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;

	for (i = 0; i < NUM_IRIS_PROFILES; i++) {
		if (iris_profile_is_10bit(iris_profiles[i]) &&
		    !dd->p010_supported)
			continue;
		if (profile_list)
			profile_list[count] = iris_profiles[i];
		count++;
	}
	*num_profiles = count;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryConfigEntrypoints(VADriverContextP ctx, VAProfile profile,
			      VAEntrypoint *entrypoint_list, int *num_entrypoints)
{
	struct iris_drv_data *dd;
	int i;

	if (!num_entrypoints)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (iris_profile_is_10bit(profile) && !dd->p010_supported) {
		*num_entrypoints = 0;
		return VA_STATUS_SUCCESS;
	}

	for (i = 0; i < (int)NUM_IRIS_PROFILES; i++) {
		if (iris_profiles[i] == profile)
			break;
	}
	if (i == (int)NUM_IRIS_PROFILES) {
		*num_entrypoints = 0;
		return VA_STATUS_SUCCESS;
	}

	*num_entrypoints = NUM_IRIS_ENTRYPOINTS;
	if (entrypoint_list) {
		if (*num_entrypoints > (int)NUM_IRIS_ENTRYPOINTS)
			*num_entrypoints = NUM_IRIS_ENTRYPOINTS;
		memcpy(entrypoint_list, iris_entrypoints,
		       *num_entrypoints * sizeof(*entrypoint_list));
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaGetConfigAttributes(VADriverContextP ctx, VAProfile profile,
			   VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
			   int num_attribs)
{
	struct iris_drv_data *dd;
	int i;

	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (!iris_profile_supported(profile) ||
	    (iris_profile_is_10bit(profile) && !dd->p010_supported))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (num_attribs < 0 || (num_attribs && !attrib_list))
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	for (i = 0; i < num_attribs; i++) {
		switch (attrib_list[i].type) {
		case VAConfigAttribRTFormat:
			attrib_list[i].value = iris_profile_rt_format(profile);
			break;
		case VAConfigAttribDecProcessing:
			attrib_list[i].value = 0;
			break;
		case VAConfigAttribMaxPictureWidth:
		case VAConfigAttribMaxPictureHeight:
			attrib_list[i].value = 4096;
			break;
		default:
			attrib_list[i].value = VA_ATTRIB_NOT_SUPPORTED;
			break;
		}
	}
	return VA_STATUS_SUCCESS;
}

static struct iris_surfs *
iris_ensure_surfs(struct iris_drv_data *dd)
{
	if (!dd->surfs)
		dd->surfs = iris_surfs_create();
	return dd->surfs;
}

static struct iris_decode_ctx *
iris_context_engine(struct iris_drv_data *dd, VAContextID context_id)
{
	int i;

	for (i = 0; i < dd->n_engines; i++)
		if (dd->engines[i].ctx_id == context_id)
			return dd->engines[i].dec;
	return NULL;
}

static struct iris_engine *
iris_context(struct iris_drv_data *dd, VAContextID context_id)
{
	int i;

	for (i = 0; i < dd->n_engines; i++)
		if (dd->engines[i].ctx_id == context_id)
			return &dd->engines[i];
	return NULL;
}

static VAStatus
iris_vaCreateConfig(VADriverContextP ctx, VAProfile profile,
		    VAEntrypoint entrypoint, VAConfigAttrib *attrib_list,
		    int num_attribs, VAConfigID *config_id)
{
	struct iris_drv_data *dd;
	int i;

	if (entrypoint != VAEntrypointVLD)
		return VA_STATUS_ERROR_UNSUPPORTED_ENTRYPOINT;
	if (!config_id)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (num_attribs < 0 || (num_attribs && !attrib_list))
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (!iris_profile_supported(profile) ||
	    (iris_profile_is_10bit(profile) && !dd->p010_supported))
		return VA_STATUS_ERROR_UNSUPPORTED_PROFILE;
	if (dd->n_configs >= IRIS_MAX_CONFIGS)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
	for (i = 0; i < num_attribs; i++) {
		if (attrib_list[i].type == VAConfigAttribRTFormat &&
		    !(attrib_list[i].value & iris_profile_rt_format(profile)))
			return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
		if (attrib_list[i].type == VAConfigAttribDecProcessing &&
		    attrib_list[i].value)
			return VA_STATUS_ERROR_UNSUPPORTED_FILTER;
	}

	*config_id = ++dd->config_id;
	dd->configs[dd->n_configs].id = *config_id;
	dd->configs[dd->n_configs].profile = profile;
	dd->n_configs++;
	fprintf(stderr, "[cfg] created config=%u profile=%d\n", *config_id,
		profile);
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroyConfig(VADriverContextP ctx, VAConfigID config_id)
{
	struct iris_drv_data *dd = ctx ? ctx->pDriverData : NULL;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	for (i = 0; i < dd->n_configs; i++) {
		if (dd->configs[i].id != config_id)
			continue;
		dd->configs[i] = dd->configs[dd->n_configs - 1];
		dd->n_configs--;
		return VA_STATUS_SUCCESS;
	}
	return VA_STATUS_ERROR_INVALID_CONFIG;
}

static VAStatus
iris_vaQueryConfigAttributes(VADriverContextP ctx, VAConfigID config_id,
			     VAProfile *profile, VAEntrypoint *entrypoint,
			     VAConfigAttrib *attrib_list, int *num_attribs)
{
	struct iris_drv_data *dd = ctx ? ctx->pDriverData : NULL;
	struct iris_config *cfg;

	if (!dd || !profile || !entrypoint || !num_attribs)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	cfg = iris_find_config(dd, config_id);
	if (!cfg)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	*profile = cfg->profile;
	*entrypoint = VAEntrypointVLD;
	if (attrib_list) {
		/* num_attribs is an output here (clients pass uninitialized
		 * garbage); always report one supported RT format. */
		attrib_list[0].type = VAConfigAttribRTFormat;
		attrib_list[0].value = iris_profile_rt_format(cfg->profile);
		*num_attribs = 1;
	} else {
		*num_attribs = 1;
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaCreateContext(VADriverContextP ctx, VAConfigID config_id, int picture_width,
		     int picture_height, int flag, VASurfaceID *render_targets,
		     int num_render_targets, VAContextID *context_id)
{
	struct iris_drv_data *dd;
	struct iris_surfs *t;
	struct iris_decode_ctx *eng;
	struct iris_config *cfg;

	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	cfg = iris_find_config(dd, config_id);
	if (!cfg)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	t = iris_ensure_surfs(dd);
	if (!t)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (dd->n_engines >= IRIS_MAX_ENGINES)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	/* One fresh engine (V4L2 session) per context: Chrome recreates the
	 * context for every stream and on resolution changes.  Reusing a
	 * session across streams leaks firmware DPB state and queued buffers,
	 * and a codec switch would feed the wrong pixfmt into a live OUTPUT
	 * queue; either way decoding breaks and Chrome falls back to
	 * software. */
	eng = iris_decode_create();
	if (!eng)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	iris_decode_setup(eng, picture_width, picture_height, cfg->profile);
	iris_decode_set_surfaces(eng, t);
	if (iris_decode_set_render_targets(eng, render_targets,
					   num_render_targets) < 0) {
		iris_decode_destroy(eng);
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	}

	dd->width = picture_width;
	dd->height = picture_height;
	dd->engines[dd->n_engines].ctx_id = ++dd->context_id;
	dd->engines[dd->n_engines].profile = cfg->profile;
	dd->engines[dd->n_engines].dec = eng;
	dd->n_engines++;
	*context_id = dd->context_id;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroyContext(VADriverContextP ctx, VAContextID context_id)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	for (i = 0; i < dd->n_engines; i++) {
		if (dd->engines[i].ctx_id != context_id)
			continue;
		/* Clean teardown between streams; sessions killed mid-flight
		 * are what wedges the firmware (SESSION_INIT timeouts until
		 * rmmod). */
		iris_decode_destroy(dd->engines[i].dec);
		dd->engines[i] = dd->engines[dd->n_engines - 1];
		dd->n_engines--;
		return VA_STATUS_SUCCESS;
	}
	return VA_STATUS_ERROR_INVALID_CONTEXT;
}

static VAStatus
iris_vaCreateSurfaces(VADriverContextP ctx, int width, int height, int format,
		      int num_surfaces, VASurfaceID *surfaces)
{
	struct iris_drv_data *dd;
	struct iris_surfs *t;
	unsigned int fourcc;
	int i = 0;

	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	t = iris_ensure_surfs(dd);
	if (!t)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if ((format & VA_RT_FORMAT_YUV420_10) && !dd->p010_supported)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
	fourcc = (format & VA_RT_FORMAT_YUV420_10) ? VA_FOURCC_P010 :
		VA_FOURCC_NV12;

	while (i < num_surfaces && dd->direct_spare_count &&
	       dd->direct_spare_width == (unsigned int)width &&
	       dd->direct_spare_height == (unsigned int)height &&
	       dd->direct_spare_fourcc == fourcc) {
		surfaces[i++] = dd->direct_spares[0];
		memmove(dd->direct_spares, dd->direct_spares + 1,
			(--dd->direct_spare_count) * sizeof(dd->direct_spares[0]));
	}

	if (i < num_surfaces && getenv("IRIS_DIRECT_CAPTURE") &&
	    !dd->direct_pool_initialized) {
		const char *slots_env = getenv("IRIS_DIRECT_CAPTURE_SLOTS");
		int requested = num_surfaces - i;
		int allocate = requested < 20 ? 20 : requested;
		int j;

		if (slots_env && *slots_env) {
			char *end;
			unsigned long slots = strtoul(slots_env, &end, 10);

			if (!*end && slots > (unsigned int)allocate &&
			    slots <= ARRAY_SIZE(dd->direct_spares))
				allocate = (int)slots;
		}

		dd->direct_pool_initialized = 1;
		dd->direct_spare_width = width;
		dd->direct_spare_height = height;
		dd->direct_spare_fourcc = fourcc;
		for (j = 0; j < allocate; j++) {
			VASurfaceID id = ++dd->surface_id;

			if (iris_surfs_alloc(t, id, width, height, fourcc))
				return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
			if (j < requested)
				surfaces[i++] = id;
			else if (dd->direct_spare_count <
				 ARRAY_SIZE(dd->direct_spares))
				dd->direct_spares[dd->direct_spare_count++] = id;
		}
	}

	for (; i < num_surfaces; i++) {
		VASurfaceID id = ++dd->surface_id;

		if (iris_surfs_alloc(t, id, width, height, fourcc))
			return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
		surfaces[i] = id;
	}

	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroySurfaces(VADriverContextP ctx, VASurfaceID *surface_list,
		       int num_surfaces)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	int i;

	if (!dd || !dd->surfs || !surface_list || num_surfaces < 0)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	for (i = 0; i < num_surfaces; i++) {
		void *surface_mem;
		unsigned int pitch, size, width, height, fourcc;
		int j = 0;

		if (iris_surfs_buffer(dd->surfs, surface_list[i], &surface_mem,
				      &pitch, &size, &width, &height, &fourcc))
			return VA_STATUS_ERROR_INVALID_SURFACE;
		/* A client should destroy derived images first.  Invalidate any that
		 * remain so vaMapBuffer can never return a dangling surface mapping. */
		while (j < dd->derived_n) {
			if (dd->derived_mem[j] != surface_mem) {
				j++;
				continue;
			}
			dd->derived_ids[j] = dd->derived_ids[dd->derived_n - 1];
			dd->derived_mem[j] = dd->derived_mem[dd->derived_n - 1];
			dd->derived_n--;
		}
		iris_surfs_free(dd->surfs, surface_list[i]);
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaCreateSurfaces2(VADriverContextP ctx, unsigned int format,
		       unsigned int width, unsigned int height,
		       VASurfaceID *surfaces, unsigned int num_surfaces,
		       VASurfaceAttrib *attrib_list, unsigned int num_attribs)
{
	unsigned int i;

	for (i = 0; i < num_attribs; i++)
		if (attrib_list[i].type == VASurfaceAttribPixelFormat &&
		    attrib_list[i].value.type == VAGenericValueTypeInteger &&
		    attrib_list[i].value.value.i == VA_FOURCC_P010)
			format = VA_RT_FORMAT_YUV420_10;
	return iris_vaCreateSurfaces(ctx, width, height, format, num_surfaces,
				     surfaces);
}

static VAStatus
iris_vaCreateBuffer(VADriverContextP ctx, VAContextID context_id,
		    VABufferType type, unsigned int size, unsigned int num_elements,
		    void *data, VABufferID *buf_id)
{
	struct iris_drv_data *dd;

	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (dd->n_bufs >= 256)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	dd->buf_types[dd->n_bufs] = type;
	dd->buf_num_elements[dd->n_bufs] = num_elements ? num_elements : 1;
	if (size > UINT_MAX / dd->buf_num_elements[dd->n_bufs])
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	dd->buf_sizes[dd->n_bufs] = size * dd->buf_num_elements[dd->n_bufs];
	dd->buf_data[dd->n_bufs] = calloc(1, dd->buf_sizes[dd->n_bufs] ?
						 dd->buf_sizes[dd->n_bufs] : 1);
	if (!dd->buf_data[dd->n_bufs])
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (data && dd->buf_sizes[dd->n_bufs])
		memcpy(dd->buf_data[dd->n_bufs], data, dd->buf_sizes[dd->n_bufs]);
	dd->buf_ids[dd->n_bufs] = ++dd->buffer_id;
	*buf_id = dd->buf_ids[dd->n_bufs];
	dd->n_bufs++;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaBufferSetNumElements(VADriverContextP ctx, VABufferID buf_id,
			    unsigned int num_elements)
{
	return VA_STATUS_SUCCESS;
}

static int
iris_find_buffer(struct iris_drv_data *dd, VABufferID buf_id)
{
	int i;

	for (i = 0; i < dd->n_bufs; i++)
		if (dd->buf_ids[i] == buf_id)
			return i;
	return -1;
}

static VAStatus
iris_vaMapBuffer(VADriverContextP ctx, VABufferID buf_id, void **pbuf)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	int i;

	*pbuf = NULL;
	if (!dd)
		return VA_STATUS_ERROR_INVALID_BUFFER;

	for (i = 0; i < dd->derived_n; i++) {
		if (dd->derived_ids[i] != buf_id)
			continue;
		*pbuf = dd->derived_mem[i];
		return VA_STATUS_SUCCESS;
	}

	for (i = 0; i < dd->img_n; i++) {
		if (dd->img_ids[i] != buf_id)
			continue;
		*pbuf = dd->img_data[i];
		return VA_STATUS_SUCCESS;
	}

	i = iris_find_buffer(dd, buf_id);
	if (i < 0)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	*pbuf = dd->buf_data[i];
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaUnmapBuffer(VADriverContextP ctx, VABufferID buf_id)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroyBuffer(VADriverContextP ctx, VABufferID buf_id)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	int i;

	if (!dd)
		return VA_STATUS_SUCCESS;
	i = iris_find_buffer(dd, buf_id);
	if (i < 0)
		return VA_STATUS_ERROR_INVALID_BUFFER;
	free(dd->buf_data[i]);
	dd->buf_ids[i] = dd->buf_ids[dd->n_bufs - 1];
	dd->buf_types[i] = dd->buf_types[dd->n_bufs - 1];
	dd->buf_sizes[i] = dd->buf_sizes[dd->n_bufs - 1];
	dd->buf_num_elements[i] = dd->buf_num_elements[dd->n_bufs - 1];
	dd->buf_data[i] = dd->buf_data[dd->n_bufs - 1];
	dd->n_bufs--;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaBeginPicture(VADriverContextP ctx, VAContextID context_id,
		    VASurfaceID render_target)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	struct iris_decode_ctx *eng;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	eng = iris_context_engine(dd, context_id);
	if (!eng)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	return iris_decode_begin(eng, render_target) ?
		VA_STATUS_ERROR_OPERATION_FAILED : VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaRenderPicture(VADriverContextP ctx, VAContextID context_id,
		     VABufferID *buffers, int num_buffers)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	struct iris_engine *engine;
	struct iris_decode_ctx *eng;
	VAProfile profile;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	engine = iris_context(dd, context_id);
	if (!engine)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	eng = engine->dec;
	profile = engine->profile;

	for (i = 0; i < num_buffers; i++) {
		int idx = iris_find_buffer(dd, buffers[i]);
		void *data;
		VABufferType type;

		if (idx < 0)
			return VA_STATUS_ERROR_INVALID_BUFFER;
		data = dd->buf_data[idx];
		type = dd->buf_types[idx];

		switch (type) {
		case VAPictureParameterBufferType:
			/* VP9 frames carry their own header, so ignore the
			 * (larger) VP9 picture parameter buffer.  HEVC parses
			 * it to re-serialize VPS/SPS/PPS. */
			if (profile == VAProfileVP9Profile0 ||
			    profile == VAProfileVP9Profile1 ||
			    profile == VAProfileVP9Profile2 ||
			    profile == VAProfileVP9Profile3)
				break;
			if (profile == VAProfileHEVCMain ||
			    profile == VAProfileHEVCMain10) {
				if (dd->buf_sizes[idx] <
				    sizeof(VAPictureParameterBufferHEVC))
					return VA_STATUS_ERROR_INVALID_BUFFER;
				if (iris_decode_hevc_picture(eng, data))
					return VA_STATUS_ERROR_INVALID_BUFFER;
				break;
			}
			if (dd->buf_sizes[idx] <
			    sizeof(VAPictureParameterBufferH264))
				return VA_STATUS_ERROR_INVALID_BUFFER;
			iris_decode_picture(eng, data);
			break;
		case VASliceParameterBufferType:
			if (profile == VAProfileVP9Profile0 ||
			    profile == VAProfileVP9Profile1 ||
			    profile == VAProfileVP9Profile2 ||
			    profile == VAProfileVP9Profile3)
				break;
			if (profile == VAProfileHEVCMain ||
			    profile == VAProfileHEVCMain10) {
				if (dd->buf_sizes[idx] <
				    sizeof(VASliceParameterBufferHEVC) *
				    dd->buf_num_elements[idx])
					return VA_STATUS_ERROR_INVALID_BUFFER;
				for (unsigned int j = 0; j < dd->buf_num_elements[idx]; j++)
					if (iris_decode_hevc_slice_params(eng,
						(const VASliceParameterBufferHEVC *)data + j))
						return VA_STATUS_ERROR_INVALID_BUFFER;
				break;
			}
			if (dd->buf_sizes[idx] < sizeof(VASliceParameterBufferH264) *
			    dd->buf_num_elements[idx])
				return VA_STATUS_ERROR_INVALID_BUFFER;
			for (unsigned int j = 0; j < dd->buf_num_elements[idx]; j++)
				iris_decode_slice_params(eng,
					(const VASliceParameterBufferH264 *)data + j);
			break;
		case VASliceDataBufferType:
			if (iris_decode_slice(eng, data, dd->buf_sizes[idx]))
				return VA_STATUS_ERROR_DECODING_ERROR;
			break;
		case VAIQMatrixBufferType:
			if (profile == VAProfileH264ConstrainedBaseline ||
			    profile == VAProfileH264Main ||
			    profile == VAProfileH264High) {
				if (dd->buf_sizes[idx] < sizeof(VAIQMatrixBufferH264) ||
				    iris_decode_h264_iq_matrix(eng, data))
					return VA_STATUS_ERROR_INVALID_BUFFER;
			} else if (profile == VAProfileHEVCMain ||
				   profile == VAProfileHEVCMain10) {
				if (dd->buf_sizes[idx] < sizeof(VAIQMatrixBufferHEVC) ||
				    iris_decode_hevc_iq_matrix(eng, data))
					return VA_STATUS_ERROR_INVALID_BUFFER;
			}
			break;
		default:
			/* Buffer types unrelated to decode bitstream assembly. */
			break;
		}
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaEndPicture(VADriverContextP ctx, VAContextID context_id)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	struct iris_decode_ctx *eng;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	eng = iris_context_engine(dd, context_id);
	if (!eng)
		return VA_STATUS_ERROR_INVALID_CONTEXT;
	return iris_decode_end(eng) ?
		VA_STATUS_ERROR_DECODING_ERROR : VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSyncSurface(VADriverContextP ctx, VASurfaceID render_target)
{
	struct iris_drv_data *dd = ctx->pDriverData;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!dd->surfs || !iris_surfs_valid(dd->surfs, render_target))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	/* Route through the registry: the picture being synced may belong to
	 * any live engine, and never-queued pool surfaces succeed without
	 * draining anything. */
	{
		int r = iris_surfs_sync(dd->surfs, render_target);

		DBG("[sync] surf=%u -> r=%d\n", render_target, r);
		if (!r)
			return VA_STATUS_SUCCESS;
		return r == -ETIMEDOUT ? VA_STATUS_ERROR_TIMEDOUT :
			VA_STATUS_ERROR_DECODING_ERROR;
	}
}

static VAStatus
iris_vaQuerySurfaceStatus(VADriverContextP ctx, VASurfaceID render_target,
			  VASurfaceStatus *status)
{
	struct iris_drv_data *dd = ctx->pDriverData;

	if (!dd || !status || !dd->surfs ||
	    !iris_surfs_valid(dd->surfs, render_target))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	*status = iris_surfs_ready(dd->surfs, render_target) ?
		VASurfaceReady : VASurfaceRendering;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryImageFormats(VADriverContextP ctx, VAImageFormat *format_list,
			 int *num_formats)
{
	struct iris_drv_data *dd;
	static const VAImageFormat nv12 = {
		.fourcc = VA_FOURCC_NV12,
		.byte_order = VA_LSB_FIRST,
		.bits_per_pixel = 12,
	};
	static const VAImageFormat p010 = {
		.fourcc = VA_FOURCC_P010,
		.byte_order = VA_LSB_FIRST,
		.bits_per_pixel = 24,
	};

	if (!num_formats)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	dd = iris_drv_data(ctx);
	if (!dd)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	if (format_list) {
		format_list[0] = nv12;
		if (dd->p010_supported)
			format_list[1] = p010;
	}
	*num_formats = dd->p010_supported ? 2 : 1;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQueryDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			      int *num_attributes)
{
	if (!num_attributes)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	*num_attributes = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaGetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			    int num_attributes)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaQuerySurfaceAttributes(VADriverContextP dpy, VAConfigID config,
			      VASurfaceAttrib *attrib_list,
			      unsigned int *num_attribs)
{
	struct iris_drv_data *dd = dpy ? dpy->pDriverData : NULL;
	struct iris_config *cfg;
	VASurfaceAttrib attrs[] = {
		{ .type = VASurfaceAttribPixelFormat,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE | VA_SURFACE_ATTRIB_SETTABLE,
		  .value.type = VAGenericValueTypeInteger,
		  .value.value.i = VA_FOURCC_NV12 },
		{ .type = VASurfaceAttribMinWidth,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE,
		  .value.type = VAGenericValueTypeInteger, .value.value.i = 16 },
		{ .type = VASurfaceAttribMaxWidth,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE,
		  .value.type = VAGenericValueTypeInteger, .value.value.i = 4096 },
		{ .type = VASurfaceAttribMinHeight,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE,
		  .value.type = VAGenericValueTypeInteger, .value.value.i = 16 },
		{ .type = VASurfaceAttribMaxHeight,
		  .flags = VA_SURFACE_ATTRIB_GETTABLE,
		  .value.type = VAGenericValueTypeInteger, .value.value.i = 4096 },
	};
	unsigned int want = ARRAY_SIZE(attrs);

	if (!num_attribs)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (!dd)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	cfg = iris_find_config(dd, config);
	if (!cfg)
		return VA_STATUS_ERROR_INVALID_CONFIG;
	attrs[0].value.value.i = iris_profile_fourcc(cfg->profile);
	if (attrib_list) {
		if (*num_attribs < want) {
			*num_attribs = want;
			return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
		}
		memcpy(attrib_list, attrs, sizeof(attrs));
		*num_attribs = want;
	} else {
		*num_attribs = want;
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaBufferInfo(VADriverContextP ctx, VABufferID buf_id,
		  VABufferType *type, unsigned int *size, unsigned int *num_elements)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

/* Image/subpicture entries are required non-NULL by libva's validation. */
static VAStatus
iris_vaCreateImage(VADriverContextP ctx, VAImageFormat *format, int width,
		   int height, VAImage *image)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	unsigned int pitch, size;
	VABufferID bid;

	if (!dd || !format || !image)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (dd->img_n >= 64)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;
	if (format->fourcc != VA_FOURCC_NV12 &&
	    format->fourcc != VA_FOURCC_P010)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;
	if (format->fourcc == VA_FOURCC_P010 && !dd->p010_supported)
		return VA_STATUS_ERROR_UNSUPPORTED_RT_FORMAT;

	/* Match the stable surface/CAPTURE layout used by the Iris backend. */
	pitch = iris_surface_pitch(width, format->fourcc);
	size = (unsigned int)pitch * ALIGN(height, 32) * 3 / 2;
	dd->img_data[dd->img_n] = calloc(1, size);
	if (!dd->img_data[dd->img_n])
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	bid = ++dd->buffer_id;
	dd->img_ids[dd->img_n] = bid;
	dd->img_sizes[dd->img_n] = size;
	dd->img_ws[dd->img_n] = width;
	dd->img_hs[dd->img_n] = height;
	dd->img_fourcc[dd->img_n] = format->fourcc;

	memset(image, 0, sizeof(*image));
	image->image_id = bid;
	image->width = width;
	image->height = height;
	image->format = *format;
	image->data_size = size;
	image->num_planes = 2;
	image->pitches[0] = pitch;
	image->pitches[1] = pitch;
	image->offsets[0] = 0;
	image->offsets[1] = (unsigned int)pitch * ALIGN(height, 32);
	image->buf = bid;
	dd->img_n++;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDeriveImage(VADriverContextP ctx, VASurfaceID surface, VAImage *image)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	unsigned int pitch, size, w, h, fourcc;
	void *mem;
	VABufferID bid;

	if (!dd || !image)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (iris_surfs_buffer(dd->surfs, surface, &mem, &pitch, &size,
			      &w, &h, &fourcc))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (dd->derived_n >= 256)
		return VA_STATUS_ERROR_MAX_NUM_EXCEEDED;

	bid = ++dd->buffer_id;
	dd->derived_ids[dd->derived_n] = bid;
	dd->derived_mem[dd->derived_n] = mem;
	dd->derived_n++;

	memset(image, 0, sizeof(*image));
	image->image_id = bid;
	image->width = w;
	image->height = h;
	image->format.fourcc = fourcc;
	image->format.byte_order = VA_LSB_FIRST;
	image->format.bits_per_pixel = fourcc == VA_FOURCC_P010 ? 24 : 12;
	image->data_size = size;
	image->num_planes = 2;
	image->pitches[0] = pitch;
	image->pitches[1] = pitch;
	image->offsets[0] = 0;
	image->offsets[1] = pitch * h;
	image->buf = bid;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDestroyImage(VADriverContextP ctx, VAImageID image)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_IMAGE;
	for (i = 0; i < dd->derived_n; i++) {
		if (dd->derived_ids[i] != image)
			continue;
		dd->derived_ids[i] = dd->derived_ids[dd->derived_n - 1];
		dd->derived_mem[i] = dd->derived_mem[dd->derived_n - 1];
		dd->derived_n--;
		return VA_STATUS_SUCCESS;
	}
	for (i = 0; i < dd->img_n; i++) {
		if (dd->img_ids[i] != image)
			continue;
		free(dd->img_data[i]);
		dd->img_ids[i] = dd->img_ids[dd->img_n - 1];
		dd->img_data[i] = dd->img_data[dd->img_n - 1];
		dd->img_sizes[i] = dd->img_sizes[dd->img_n - 1];
		dd->img_ws[i] = dd->img_ws[dd->img_n - 1];
		dd->img_hs[i] = dd->img_hs[dd->img_n - 1];
		dd->img_fourcc[i] = dd->img_fourcc[dd->img_n - 1];
		dd->img_n--;
		return VA_STATUS_SUCCESS;
	}
	return VA_STATUS_ERROR_INVALID_IMAGE;
}

static VAStatus
iris_vaSetImagePalette(VADriverContextP ctx, VAImageID image,
		       unsigned char *palette)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaGetImage(VADriverContextP ctx, VASurfaceID surface, int x, int y,
		unsigned int width, unsigned int height, VAImageID image)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	unsigned int pitch, size, cap_w, cap_h, fourcc;
	int img_i = -1;
	void *mem, *dst = NULL;
	int i;

	if (!dd)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	if (!dd->surfs || !iris_surfs_valid(dd->surfs, surface))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	i = iris_surfs_sync(dd->surfs, surface);
	if (i)
		return i == -ETIMEDOUT ? VA_STATUS_ERROR_TIMEDOUT :
			VA_STATUS_ERROR_DECODING_ERROR;
	if (iris_surfs_buffer(dd->surfs, surface, &mem, &pitch, &size,
			      &cap_w, &cap_h, &fourcc))
		return VA_STATUS_ERROR_INVALID_SURFACE;
	for (i = 0; i < dd->img_n; i++)
		if (dd->img_ids[i] == image) {
			dst = dd->img_data[i];
			img_i = i;
			break;
		}
	if (!dst)
		return VA_STATUS_ERROR_INVALID_IMAGE;
	if (dd->img_fourcc[img_i] != fourcc)
		return VA_STATUS_ERROR_OPERATION_FAILED;

	/* The image was created with the aligned coded layout; the buffer may
	 * carry the CAPTURE-negotiated one.  Copy row-wise between them so
	 * diverging strides cannot garble rows or overrun the image. */
	{
		unsigned int img_w = dd->img_ws[img_i];
		unsigned int img_h = dd->img_hs[img_i];
		unsigned int img_pitch = iris_surface_pitch(img_w, fourcc);
		unsigned int img_chroma = img_pitch * ALIGN(img_h, 32);
		unsigned int bytes = fourcc == VA_FOURCC_P010 ? 2 : 1;
		unsigned int row_bytes;
		const uint8_t *src;
		uint8_t *d = dst;

		if (x < 0 || y < 0 || (x | y | width | height) & 1 ||
		    width > img_w || height > img_h ||
		    (unsigned int)x > cap_w || (unsigned int)y > cap_h ||
		    width > cap_w - (unsigned int)x ||
		    height > cap_h - (unsigned int)y)
			return VA_STATUS_ERROR_INVALID_PARAMETER;
		row_bytes = width * bytes;
		src = (const uint8_t *)mem + (unsigned int)y * pitch +
		      (unsigned int)x * bytes;
		for (i = 0; i < (int)height; i++)
			memcpy(d + (unsigned int)i * img_pitch,
			       src + (unsigned int)i * pitch, row_bytes);
		src = (const uint8_t *)mem + pitch * cap_h +
		      (unsigned int)(y / 2) * pitch + (unsigned int)x * bytes;
		d = dst + img_chroma;
		for (i = 0; i < (int)(height / 2); i++)
			memcpy(d + (unsigned int)i * img_pitch,
			       src + (unsigned int)i * pitch, row_bytes);
	}
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaPutImage(VADriverContextP ctx, VASurfaceID surface, VAImageID image,
		int src_x, int src_y, unsigned int src_width,
		unsigned int src_height, int dest_x, int dest_y,
		unsigned int dest_width, unsigned int dest_height)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaQuerySubpictureFormats(VADriverContextP ctx, VAImageFormat *format_list,
			      unsigned int *flags, unsigned int *num_formats)
{
	if (num_formats)
		*num_formats = 0;
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaCreateSubpicture(VADriverContextP ctx, VAImageID image,
			VASubpictureID *subpicture)
{
	return VA_STATUS_ERROR_UNIMPLEMENTED;
}

static VAStatus
iris_vaDestroySubpicture(VADriverContextP ctx, VASubpictureID subpicture)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetSubpictureImage(VADriverContextP ctx, VASubpictureID subpicture,
			  VAImageID image)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetSubpictureChromakey(VADriverContextP ctx, VASubpictureID subpicture,
			      unsigned int chromakey_min,
			      unsigned int chromakey_max,
			      unsigned int chromakey_mask)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetSubpictureGlobalAlpha(VADriverContextP ctx, VASubpictureID subpicture,
				float global_alpha)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaAssociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture,
			   VASurfaceID *target_surfaces, int num_surfaces,
			   short src_x, short src_y, unsigned short src_width,
			   unsigned short src_height, short dest_x, short dest_y,
			   unsigned short dest_width, unsigned short dest_height,
			   unsigned int flags)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaDeassociateSubpicture(VADriverContextP ctx, VASubpictureID subpicture,
			     VASurfaceID *target_surfaces, int num_surfaces)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaSetDisplayAttributes(VADriverContextP ctx, VADisplayAttribute *attr_list,
			    int num_attributes)
{
	return VA_STATUS_SUCCESS;
}

static VAStatus
iris_vaExportSurfaceHandle(VADriverContextP ctx, VASurfaceID surface_id,
			   uint32_t mem_type, uint32_t flags, void *descriptor)
{
	struct iris_drv_data *dd = ctx->pDriverData;
	VADRMPRIMESurfaceDescriptor *d = descriptor;
	unsigned int w, h, pitch, size, fourcc;
	uint32_t layer_flags;
	int fd;

	if (mem_type != VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2)
		return VA_STATUS_ERROR_UNSUPPORTED_BUFFERTYPE;
	if (!dd || !dd->surfs)
		return VA_STATUS_ERROR_INVALID_SURFACE;
	if (!descriptor)
		return VA_STATUS_ERROR_INVALID_PARAMETER;
	layer_flags = flags & (VA_EXPORT_SURFACE_SEPARATE_LAYERS |
			       VA_EXPORT_SURFACE_COMPOSED_LAYERS);
	if (layer_flags != VA_EXPORT_SURFACE_SEPARATE_LAYERS &&
	    layer_flags != VA_EXPORT_SURFACE_COMPOSED_LAYERS)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	DBG("[export] surf=%u type=%u flags=0x%x\n", surface_id,
	    mem_type, flags);
	if (iris_surfs_export(dd->surfs, surface_id, &fd, &pitch, &size,
			      &w, &h, &fourcc))
		return VA_STATUS_ERROR_INVALID_SURFACE;

	memset(d, 0, sizeof(*d));
	d->fourcc = fourcc;
	d->width = w;
	d->height = h;
	d->num_objects = 1;
	d->objects[0].fd = fd;
	d->objects[0].size = size;
	d->objects[0].drm_format_modifier = DRM_FORMAT_MOD_LINEAR;
	if (layer_flags == VA_EXPORT_SURFACE_COMPOSED_LAYERS) {
		d->num_layers = 1;
		d->layers[0].drm_format = fourcc == VA_FOURCC_P010 ?
			DRM_FORMAT_P010 : DRM_FORMAT_NV12;
		d->layers[0].num_planes = 2;
		d->layers[0].object_index[0] = 0;
		d->layers[0].object_index[1] = 0;
		d->layers[0].offset[0] = 0;
		d->layers[0].offset[1] = pitch * h;
		d->layers[0].pitch[0] = pitch;
		d->layers[0].pitch[1] = pitch;
	} else {
		/* Chrome requests separate layers and requires one plane per layer. */
		d->num_layers = 2;
		d->layers[0].drm_format = fourcc == VA_FOURCC_P010 ?
			DRM_FORMAT_R16 : DRM_FORMAT_R8;
		d->layers[0].num_planes = 1;
		d->layers[0].object_index[0] = 0;
		d->layers[0].offset[0] = 0;
		d->layers[0].pitch[0] = pitch;
		d->layers[1].drm_format = fourcc == VA_FOURCC_P010 ?
			DRM_FORMAT_GR1616 : DRM_FORMAT_GR88;
		d->layers[1].num_planes = 1;
		d->layers[1].object_index[0] = 0;
		d->layers[1].offset[0] = pitch * h;
		d->layers[1].pitch[0] = pitch;
	}
	return VA_STATUS_SUCCESS;
}

static const struct VADriverVTable iris_vtable_template = {
	.vaTerminate = iris_vaTerminate,
	.vaQueryConfigProfiles = iris_vaQueryConfigProfiles,
	.vaQueryConfigEntrypoints = iris_vaQueryConfigEntrypoints,
	.vaGetConfigAttributes = iris_vaGetConfigAttributes,
	.vaCreateConfig = iris_vaCreateConfig,
	.vaDestroyConfig = iris_vaDestroyConfig,
	.vaQueryConfigAttributes = iris_vaQueryConfigAttributes,
	.vaCreateSurfaces = iris_vaCreateSurfaces,
	.vaDestroySurfaces = iris_vaDestroySurfaces,
	.vaCreateContext = iris_vaCreateContext,
	.vaDestroyContext = iris_vaDestroyContext,
	.vaCreateBuffer = iris_vaCreateBuffer,
	.vaBufferSetNumElements = iris_vaBufferSetNumElements,
	.vaMapBuffer = iris_vaMapBuffer,
	.vaUnmapBuffer = iris_vaUnmapBuffer,
	.vaDestroyBuffer = iris_vaDestroyBuffer,
	.vaBeginPicture = iris_vaBeginPicture,
	.vaRenderPicture = iris_vaRenderPicture,
	.vaEndPicture = iris_vaEndPicture,
	.vaSyncSurface = iris_vaSyncSurface,
	.vaQuerySurfaceStatus = iris_vaQuerySurfaceStatus,
	.vaQueryImageFormats = iris_vaQueryImageFormats,
	.vaCreateImage = iris_vaCreateImage,
	.vaDeriveImage = iris_vaDeriveImage,
	.vaDestroyImage = iris_vaDestroyImage,
	.vaSetImagePalette = iris_vaSetImagePalette,
	.vaGetImage = iris_vaGetImage,
	.vaPutImage = iris_vaPutImage,
	.vaQuerySubpictureFormats = iris_vaQuerySubpictureFormats,
	.vaCreateSubpicture = iris_vaCreateSubpicture,
	.vaDestroySubpicture = iris_vaDestroySubpicture,
	.vaSetSubpictureImage = iris_vaSetSubpictureImage,
	.vaSetSubpictureChromakey = iris_vaSetSubpictureChromakey,
	.vaSetSubpictureGlobalAlpha = iris_vaSetSubpictureGlobalAlpha,
	.vaAssociateSubpicture = iris_vaAssociateSubpicture,
	.vaDeassociateSubpicture = iris_vaDeassociateSubpicture,
	.vaQueryDisplayAttributes = iris_vaQueryDisplayAttributes,
	.vaGetDisplayAttributes = iris_vaGetDisplayAttributes,
	.vaSetDisplayAttributes = iris_vaSetDisplayAttributes,
	.vaBufferInfo = iris_vaBufferInfo,
	.vaCreateSurfaces2 = iris_vaCreateSurfaces2,
	.vaQuerySurfaceAttributes = iris_vaQuerySurfaceAttributes,
	.vaExportSurfaceHandle = iris_vaExportSurfaceHandle,
};

VAStatus
__vaDriverInit_1_23(VADriverContextP ctx, int major_version, int minor_version)
{
	struct VADriverVTable *vt;

	if (!ctx)
		return VA_STATUS_ERROR_INVALID_PARAMETER;

	/*
	 * libva owns ctx->vtable after init and frees it in vaTerminate(), so
	 * hand it a heap copy of the static template rather than the template
	 * itself (which lives in .rodata and cannot be freed).
	 */
	vt = calloc(1, sizeof(*vt));
	if (!vt)
		return VA_STATUS_ERROR_ALLOCATION_FAILED;
	*vt = iris_vtable_template;

	ctx->version_major = major_version;
	ctx->version_minor = minor_version;
	ctx->vtable = vt;
	ctx->max_profiles = NUM_IRIS_PROFILES;
	ctx->max_entrypoints = NUM_IRIS_ENTRYPOINTS;
	ctx->max_attributes = 1;
	ctx->max_image_formats = 2;
	ctx->max_subpic_formats = 1;
	ctx->max_display_attributes = 0;
	ctx->str_vendor = IRIS_VAAPI_VENDOR IRIS_VAAPI_VERSION;
	ctx->pDriverData = NULL;

	{
		struct iris_drv_data *dd = iris_drv_data(ctx);

		if (!dd)
			return VA_STATUS_ERROR_ALLOCATION_FAILED;
		dd->p010_supported = v4l2_dec_supports_capture_format(
			NULL, V4L2_PIX_FMT_P010);
		if (!dd->p010_supported)
			fprintf(stderr,
				"iris-vaapi: kernel does not advertise P010; hiding HEVC Main10 and VP9 Profile 2\n");
	}

	return VA_STATUS_SUCCESS;
}

/* Compatibility aliases for older libva releases */
VAStatus
vaDriverInit(VADriverContextP ctx, int major_version, int minor_version)
{
	return __vaDriverInit_1_23(ctx, major_version, minor_version);
}
