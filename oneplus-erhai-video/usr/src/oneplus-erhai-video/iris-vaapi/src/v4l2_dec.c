// SPDX-License-Identifier: GPL-2.0-or-later
/* Minimal stateful V4L2 M2M decoder engine for the Qualcomm Iris device.
 *
 * Feeds whole H.264/HEVC/VP9 access units to the OUTPUT (bitstream) queue and
 * returns decoded NV12 frames from the CAPTURE queue.  The CAPTURE queue is
 * set up only after the decoder reports the stream resolution through
 * V4L2_EVENT_SOURCE_CHANGE, mirroring the FFmpeg v4l2-m2m flow.
 */

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <stdint.h>

#include "v4l2_dec.h"
#include "iris_surface_fence.h"

#define OUT_BUFFERS	16
#define CAP_BUFFERS	20

static unsigned int
out_sizeimage(unsigned int width, unsigned int height)
{
	uint64_t pixels = (uint64_t)width * height;
	uint64_t size = pixels * 2; /* generous compressed-frame working size */

	if (size < 1U * 1024 * 1024)
		size = 1U * 1024 * 1024;
	if (size > UINT32_MAX)
		size = UINT32_MAX;
	return (unsigned int)size;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
	int r;

	do {
		r = ioctl(fd, req, arg);
	} while (r == -1 && errno == EINTR);

	return r;
}

/* Resolve the Iris decoder V4L2 node: $IRIS_VAAPI_DEVICE if set, else scan
 * /dev/video* for the M2M mplane node whose driver is "iris_driver". The
 * node index is NOT stable across boots (camera VFE nodes may claim
 * video0/video1 depending on bring-up order), so never hardcode it. */
char *v4l2_dec_find_device(void)
{
	const char *env = getenv("IRIS_VAAPI_DEVICE");
	char path[64];
	struct v4l2_capability cap;
	DIR *dir;
	struct dirent *e;
	int fd, best = -1;

	if (env && *env)
		return strdup(env);

	dir = opendir("/dev");
	if (!dir)
		return NULL;

	while ((e = readdir(dir))) {
		if (strncmp(e->d_name, "video", 5) != 0)
			continue;

		snprintf(path, sizeof(path), "/dev/%s", e->d_name);
		fd = open(path, O_RDWR | O_NONBLOCK);
		if (fd < 0)
			continue;

		if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
		    (cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE) &&
		    strcmp((char *)cap.driver, "iris_driver") == 0 &&
		    strcmp((char *)cap.card, "Iris Decoder") == 0)
			best = atoi(e->d_name + 5);

		close(fd);

		if (best >= 0)
			break;
	}
	closedir(dir);

	if (best < 0)
		return NULL;

	snprintf(path, sizeof(path), "/dev/video%d", best);
	printf("v4l2-dec: discovered iris decoder at %s\n", path);
	return strdup(path);
}

int v4l2_dec_supports_capture_format(const char *dev,
				      unsigned int pixelformat)
{
	struct v4l2_fmtdesc fmt;
	int fd;

	fd = open(dev ? dev : (dev = v4l2_dec_find_device()), O_RDWR | O_NONBLOCK);
	if (fd < 0)
		return 0;

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	while (xioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0) {
		if (fmt.pixelformat == pixelformat) {
			close(fd);
			return 1;
		}
		fmt.index++;
	}
	close(fd);
	return 0;
}

int v4l2_dec_attach_surface_fence(struct v4l2_dec *d, int dmabuf_fd,
				  uint64_t token)
{
	struct iris_surface_fence_cmd cmd = {
		.op = IRIS_SURFACE_FENCE_ATTACH,
		.dmabuf_fd = dmabuf_fd,
		.token = token,
	};

	return xioctl(d->fd, VIDIOC_IRIS_SURFACE_FENCE, &cmd) < 0 ?
		-errno : 0;
}

int v4l2_dec_signal_surface_fence(struct v4l2_dec *d, uint64_t token)
{
	struct iris_surface_fence_cmd cmd = {
		.op = IRIS_SURFACE_FENCE_SIGNAL,
		.dmabuf_fd = -1,
		.token = token,
	};

	return xioctl(d->fd, VIDIOC_IRIS_SURFACE_FENCE, &cmd) < 0 ?
		-errno : 0;
}

static void v4l2_dec_enable_decode_order(struct v4l2_dec *d)
{
	struct v4l2_control ctrl = {
		.id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY,
		.value = 0,
	};
	const char *env = getenv("IRIS_NO_DECODE_ORDER");

	/* Debug/repro switch: run exactly like clients that never set the
	 * display-delay controls (e.g. chromium's V4L2VideoDecoder). */
	if (env && *env) {
		printf("v4l2-dec: decode-order output DISABLED (IRIS_NO_DECODE_ORDER)\n");
		return;
	}

	if (xioctl(d->fd, VIDIOC_S_CTRL, &ctrl) < 0)
		goto unsupported;

	ctrl.id = V4L2_CID_MPEG_VIDEO_DEC_DISPLAY_DELAY_ENABLE;
	ctrl.value = 1;
	if (xioctl(d->fd, VIDIOC_S_CTRL, &ctrl) < 0)
		goto unsupported;

	printf("v4l2-dec: firmware decode-order output enabled\n");
	return;

unsupported:
	fprintf(stderr,
		"v4l2-dec: firmware decode-order output unavailable: %s\n",
		strerror(errno));
}

static int v4l2_dec_mmap(struct v4l2_dec *d, enum v4l2_buf_type type)
{
	struct v4l2_buffer *meta;
	void ***mem;
	size_t **size;
	unsigned int count, i;

	if (type == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE) {
		meta = d->out_meta;
		mem = &d->out_mem;
		size = &d->out_size;
		count = d->out_count;
	} else {
		meta = d->cap_meta;
		mem = &d->cap_mem;
		size = &d->cap_size;
		count = d->cap_count;
	}

	*mem = calloc(count, sizeof(void *));
	if (!*size)
		*size = calloc(count, sizeof(size_t));
	if (!*mem || !*size)
		return -ENOMEM;

	for (i = 0; i < count; i++) {
		void *addr;

		addr = mmap(NULL, meta[i].m.planes[0].length,
			    PROT_READ | PROT_WRITE, MAP_SHARED, d->fd,
			    meta[i].m.planes[0].m.mem_offset);
		if (addr == MAP_FAILED)
			return -errno;
		(*mem)[i] = addr;
		(*size)[i] = meta[i].m.planes[0].length;
	}

	return 0;
}

/* Set up (or re-set-up) the CAPTURE queue using the driver's current format,
 * which is the default before the first DRC event and the real resolution
 * afterwards. */
static int v4l2_dec_setup_capture(struct v4l2_dec *d)
{
	struct v4l2_format fmt;
	struct v4l2_requestbuffers req;
	enum v4l2_memory memory = d->cap_dmabuf_count ?
		V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
	unsigned int i;

	/* Drop any previous CAPTURE buffers. */
	if (d->cap_meta) {
		if (d->streaming_cap) {
			enum v4l2_buf_type type =
				V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			xioctl(d->fd, VIDIOC_STREAMOFF, &type);
			d->streaming_cap = 0;
		}
		for (i = 0; i < d->cap_count; i++) {
			if (d->cap_mem && d->cap_mem[i])
				munmap(d->cap_mem[i], d->cap_size[i]);
			free(d->cap_meta[i].m.planes);
		}
		free(d->cap_mem);
		free(d->cap_size);
		free(d->cap_queued);
		free(d->cap_meta);
		d->cap_mem = NULL;
		d->cap_size = NULL;
		d->cap_queued = NULL;
		d->cap_meta = NULL;
		d->cap_count = 0;

		memset(&req, 0, sizeof(req));
		req.count = 0;
		req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		req.memory = d->cap_memory;
		xioctl(d->fd, VIDIOC_REQBUFS, &req);
	}

	memset(&fmt, 0, sizeof(fmt));
	fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	fmt.fmt.pix_mp.width = d->width;
	fmt.fmt.pix_mp.height = d->height;
	fmt.fmt.pix_mp.pixelformat = d->cap_pixfmt;
	fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(d->fd, VIDIOC_S_FMT, &fmt) < 0) {
		perror("S_FMT CAPTURE");
		return -errno;
	}
	d->cap_fmt = fmt;
	d->cap_pixfmt = fmt.fmt.pix_mp.pixelformat;
	/* Keep the negotiated capture geometry, not merely the requested one.
	 * This matters after a V4L2 source-change event and for codecs whose
	 * coded dimensions are aligned differently at different resolutions. */
	d->width = fmt.fmt.pix_mp.width;
	d->height = fmt.fmt.pix_mp.height;

	memset(&req, 0, sizeof(req));
	req.count = d->cap_dmabuf_count ? d->cap_dmabuf_count : CAP_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	req.memory = memory;
	if (xioctl(d->fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS CAPTURE");
		return -errno;
	}
	/* Slot N is permanently associated with direct surface N.  Accepting a
	 * smaller queue would leave valid VA targets without a V4L2 slot. */
	if (memory == V4L2_MEMORY_DMABUF && req.count != d->cap_dmabuf_count)
		return -EINVAL;
	d->cap_count = req.count;
	d->cap_generation++;
	d->cap_memory = memory;
	d->cap_meta = calloc(d->cap_count, sizeof(*d->cap_meta));
	d->cap_size = calloc(d->cap_count, sizeof(*d->cap_size));
	d->cap_queued = calloc(d->cap_count, sizeof(*d->cap_queued));
	if (!d->cap_meta || !d->cap_size || !d->cap_queued)
		return -ENOMEM;
	for (i = 0; i < d->cap_count; i++) {
		memset(&d->cap_meta[i], 0, sizeof(d->cap_meta[i]));
		d->cap_meta[i].type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		d->cap_meta[i].memory = memory;
		d->cap_meta[i].index = i;
		d->cap_meta[i].m.planes = calloc(1, sizeof(struct v4l2_plane));
		d->cap_meta[i].length = 1;
		if (memory == V4L2_MEMORY_DMABUF) {
			d->cap_meta[i].m.planes[0].m.fd = d->cap_dmabuf_fds[i];
			d->cap_meta[i].m.planes[0].length =
				d->cap_dmabuf_sizes[i];
			d->cap_size[i] = d->cap_dmabuf_sizes[i];
		} else if (xioctl(d->fd, VIDIOC_QUERYBUF,
				  &d->cap_meta[i]) < 0) {
			perror("QUERYBUF CAPTURE");
			return -errno;
		} else {
			d->cap_size[i] = d->cap_meta[i].m.planes[0].length;
		}
	}
	if (memory == V4L2_MEMORY_MMAP &&
	    v4l2_dec_mmap(d, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE) < 0) {
		perror("mmap CAPTURE");
		return -ENOMEM;
	}

	for (i = 0; i < d->cap_count; i++) {
		memset(&d->cap_meta[i].m.planes[0], 0,
		       sizeof(d->cap_meta[i].m.planes[0]));
		d->cap_meta[i].m.planes[0].bytesused = 0;
		if (memory == V4L2_MEMORY_DMABUF) {
			d->cap_meta[i].m.planes[0].m.fd = d->cap_dmabuf_fds[i];
			d->cap_meta[i].m.planes[0].length =
				d->cap_dmabuf_sizes[i];
		}
		if (xioctl(d->fd, VIDIOC_QBUF, &d->cap_meta[i]) < 0) {
			perror("QBUF CAPTURE");
			return -errno;
		}
		d->cap_queued[i] = 1;
	}

	{
		enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

		if (xioctl(d->fd, VIDIOC_STREAMON, &type) < 0)
			return -errno;
		d->streaming_cap = 1;
	}

	printf("v4l2-dec: CAPTURE %ux%u sizeimage=%u bufs=%u memory=%s\n",
	       d->width, d->height, fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
	       d->cap_count, memory == V4L2_MEMORY_DMABUF ? "DMABUF" : "MMAP");
	return 0;
}

int v4l2_dec_open(struct v4l2_dec *d, const char *dev,
		  unsigned int width, unsigned int height,
		  unsigned int pixelformat, unsigned int cap_pixelformat)
{
	struct v4l2_capability cap;
	struct v4l2_format cap_fmt;
	struct v4l2_requestbuffers req;
	struct v4l2_event_subscription sub;
	unsigned int i;
	int ret;

	memset(d, 0, sizeof(*d));
	d->cap_memory = V4L2_MEMORY_MMAP;
	d->fd = -1;
	d->fd = open(dev ? dev : (dev = v4l2_dec_find_device()), O_RDWR | O_NONBLOCK);
	if (d->fd < 0) {
		perror(dev ? dev : "open iris decoder");
		ret = d->fd < 0 ? -errno : -ENOENT;
		goto error;
	}

	if (xioctl(d->fd, VIDIOC_QUERYCAP, &cap) < 0) {
		perror("QUERYCAP");
		ret = -errno;
		goto error;
	}
	if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
		fprintf(stderr, "not an M2M mplane device: %s\n",
			(char *)cap.driver);
		ret = -EINVAL;
		goto error;
	}
	printf("v4l2-dec: device %s driver '%s' card '%s'\n", dev,
	       (char *)cap.driver, (char *)cap.card);

	memset(&sub, 0, sizeof(sub));
	sub.type = V4L2_EVENT_SOURCE_CHANGE;
	if (xioctl(d->fd, VIDIOC_SUBSCRIBE_EVENT, &sub) < 0)
		perror("SUBSCRIBE_EVENT SOURCE_CHANGE");

	d->width = width;
	d->height = height;
	d->cap_pixfmt = cap_pixelformat;

	/* ---- OUTPUT (bitstream) ---- */
	memset(&d->out_fmt, 0, sizeof(d->out_fmt));
	d->out_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	d->out_fmt.fmt.pix_mp.width = width;
	d->out_fmt.fmt.pix_mp.height = height;
	d->out_fmt.fmt.pix_mp.pixelformat = pixelformat;
	d->out_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	d->out_fmt.fmt.pix_mp.num_planes = 1;
	d->out_fmt.fmt.pix_mp.plane_fmt[0].sizeimage =
		out_sizeimage(width, height);
	if (xioctl(d->fd, VIDIOC_S_FMT, &d->out_fmt) < 0) {
		perror("S_FMT OUTPUT");
		ret = -errno;
		goto error;
	}

	/* Select the raw output before OUTPUT STREAMON.  HFI Gen1 applies the
	 * NV12/P010 and internal UBWC format pair while starting the compressed
	 * queue; changing CAPTURE only after the first bit-depth event is too
	 * late for 10-bit streams. */
	memset(&cap_fmt, 0, sizeof(cap_fmt));
	cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	cap_fmt.fmt.pix_mp.width = width;
	cap_fmt.fmt.pix_mp.height = height;
	cap_fmt.fmt.pix_mp.pixelformat = cap_pixelformat;
	cap_fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
	cap_fmt.fmt.pix_mp.num_planes = 1;
	if (xioctl(d->fd, VIDIOC_S_FMT, &cap_fmt) < 0) {
		perror("S_FMT CAPTURE preconfigure");
		ret = -errno;
		goto error;
	}
	if (cap_fmt.fmt.pix_mp.pixelformat != cap_pixelformat) {
		fprintf(stderr, "CAPTURE format %#x rejected (got %#x)\n",
			cap_pixelformat, cap_fmt.fmt.pix_mp.pixelformat);
		ret = -ENOTSUP;
		goto error;
	}

	/* Chrome can release a VA surface for presentation before a stateful
	 * decoder has emitted that frame in display order.  Decode-order output
	 * makes reference frames available early enough for the driver's stable
	 * exported backing store to be populated before presentation. */
	v4l2_dec_enable_decode_order(d);

	memset(&req, 0, sizeof(req));
	req.count = OUT_BUFFERS;
	req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	req.memory = V4L2_MEMORY_MMAP;
	if (xioctl(d->fd, VIDIOC_REQBUFS, &req) < 0) {
		perror("REQBUFS OUTPUT");
		ret = -errno;
		goto error;
	}
	d->out_count = req.count;
	d->out_meta = calloc(d->out_count, sizeof(*d->out_meta));
	if (!d->out_meta) {
		ret = -ENOMEM;
		goto error;
	}
	d->free_out = malloc(d->out_count * sizeof(*d->free_out));
	if (!d->free_out) {
		ret = -ENOMEM;
		goto error;
	}
	for (i = 0; i < d->out_count; i++)
		d->free_out[i] = d->out_count - 1 - i;
	d->free_out_n = d->out_count;
	for (i = 0; i < d->out_count; i++) {
		memset(&d->out_meta[i], 0, sizeof(d->out_meta[i]));
		d->out_meta[i].type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		d->out_meta[i].memory = V4L2_MEMORY_MMAP;
		d->out_meta[i].index = i;
		d->out_meta[i].m.planes = calloc(1, sizeof(struct v4l2_plane));
		d->out_meta[i].length = 1;
		if (!d->out_meta[i].m.planes) {
			ret = -ENOMEM;
			goto error;
		}
		if (xioctl(d->fd, VIDIOC_QUERYBUF, &d->out_meta[i]) < 0) {
			ret = -errno;
			goto error;
		}
	}
	ret = v4l2_dec_mmap(d, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (ret)
		goto error;

	return 0;

error:
	v4l2_dec_close(d);
	return ret;
}

int v4l2_dec_set_capture_dmabufs(struct v4l2_dec *d, const int *fds,
				 const size_t *sizes, unsigned int count)
{
	if (!d || !fds || !sizes || count < 4 || d->streaming || d->cap_meta)
		return -EINVAL;
	d->cap_dmabuf_fds = malloc(count * sizeof(*d->cap_dmabuf_fds));
	d->cap_dmabuf_sizes = malloc(count * sizeof(*d->cap_dmabuf_sizes));
	if (!d->cap_dmabuf_fds || !d->cap_dmabuf_sizes) {
		free(d->cap_dmabuf_fds);
		free(d->cap_dmabuf_sizes);
		d->cap_dmabuf_fds = NULL;
		d->cap_dmabuf_sizes = NULL;
		return -ENOMEM;
	}
	memcpy(d->cap_dmabuf_fds, fds, count * sizeof(*fds));
	memcpy(d->cap_dmabuf_sizes, sizes, count * sizeof(*sizes));
	d->cap_dmabuf_count = count;
	return 0;
}

static int v4l2_dec_wait_first_source_change(struct v4l2_dec *d)
{
	struct pollfd pfd = {
		.fd = d->fd,
		.events = POLLPRI,
	};
	struct v4l2_event ev;
	struct v4l2_format fmt;
	int ret;

	for (;;) {
		do {
			ret = poll(&pfd, 1, 3000);
		} while (ret < 0 && errno == EINTR);
		if (ret == 0)
			return -ETIMEDOUT;
		if (ret < 0)
			return -errno;

		memset(&ev, 0, sizeof(ev));
		if (xioctl(d->fd, VIDIOC_DQEVENT, &ev) < 0) {
			if (errno == EAGAIN || errno == ENOENT)
				continue;
			return -errno;
		}
		if (ev.type != V4L2_EVENT_SOURCE_CHANGE)
			continue;

		memset(&fmt, 0, sizeof(fmt));
		fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (xioctl(d->fd, VIDIOC_G_FMT, &fmt) < 0)
			return -errno;
		d->width = fmt.fmt.pix_mp.width;
		d->height = fmt.fmt.pix_mp.height;
		return 0;
	}
}

int v4l2_dec_start(struct v4l2_dec *d)
{
	enum v4l2_buf_type type;
	int ret;

	type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	if (xioctl(d->fd, VIDIOC_STREAMON, &type) < 0)
		return -errno;
	d->streaming = 1;

	/* A stateful decoder does not know its decoded layout until the first
	 * compressed access unit has been parsed.  Starting CAPTURE immediately
	 * races that sequence event; legacy Venus can enter a global fatal state
	 * if SESSION_START arrives while the instance is still in FIRST_IPSC
	 * (substate 0x80). */
	ret = v4l2_dec_wait_first_source_change(d);
	if (ret)
		return ret;
	return v4l2_dec_setup_capture(d);
}

/* Poll the device for any progress (CAPTURE frames, events, OUTPUT space).
 *
 * Used by feed/drain loops: the OUTPUT queue being writable must wake the
 * caller promptly so consumed bitstream buffers get recycled, otherwise the
 * pipeline stalls waiting for frames that need more input first.
 */
int v4l2_dec_poll(struct v4l2_dec *d, int timeout_ms)
{
	struct pollfd pfd;

	pfd.fd = d->fd;
	pfd.events = POLLIN | POLLPRI | POLLOUT;
	pfd.revents = 0;
	return poll(&pfd, 1, timeout_ms);
}

/* Poll waiting specifically for a decoded CAPTURE frame or an event.
 *
 * Only POLLIN|POLLPRI: adding POLLOUT here made vaSyncSurface-style waiters
 * spin through their whole retry budget in microseconds (OUTPUT is almost
 * always writable) and report a timeout while the frame was still in flight.
 */
int v4l2_dec_poll_cap(struct v4l2_dec *d, int timeout_ms)
{
	struct pollfd pfd;

	pfd.fd = d->fd;
	pfd.events = POLLIN | POLLPRI;
	pfd.revents = 0;
	return poll(&pfd, 1, timeout_ms);
}

int v4l2_dec_feed(struct v4l2_dec *d, const void *data, size_t len,
		  uint64_t timestamp)
{
	unsigned int idx;
	struct v4l2_buffer *b;

	if (d->free_out_n <= 0)
		return -EAGAIN;
	idx = d->free_out[--d->free_out_n];

	if (len == 0 || len > d->out_size[idx]) {
		fprintf(stderr, "v4l2-dec: access unit too large (%zu > %zu)\n",
			len, d->out_size[idx]);
		d->free_out[d->free_out_n++] = idx;
		return -E2BIG;
	}

	memcpy(d->out_mem[idx], data, len);
	b = &d->out_meta[idx];
	memset(&b->m.planes[0], 0, sizeof(b->m.planes[0]));
	b->m.planes[0].bytesused = len;
	b->timestamp.tv_sec = timestamp / 1000000000ULL;
	b->timestamp.tv_usec = (timestamp % 1000000000ULL) / 1000ULL;
	if (xioctl(d->fd, VIDIOC_QBUF, b) < 0) {
		d->free_out[d->free_out_n++] = idx;
		return -errno;
	}

	return 0;
}

/* Handle a DRC event if one is pending; returns 1 if CAPTURE was (re)built. */
int v4l2_dec_handle_events(struct v4l2_dec *d, int *changed)
{
	struct v4l2_event ev;
	int ret;

	*changed = 0;
	for (;;) {
		ret = xioctl(d->fd, VIDIOC_DQEVENT, &ev);
		if (ret < 0) {
			if (errno == EAGAIN || errno == ENOENT ||
			    errno == ENODEV)
				break;
			return -errno;
		}

		if (ev.type == V4L2_EVENT_SOURCE_CHANGE) {
			struct v4l2_format fmt;

			/* The first DRC report usually matches the CAPTURE format
			 * we already requested; only renegotiate on a real
			 * resolution change. */
			memset(&fmt, 0, sizeof(fmt));
			fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
			if (xioctl(d->fd, VIDIOC_G_FMT, &fmt) == 0 &&
			    fmt.fmt.pix_mp.width ==
					 d->cap_fmt.fmt.pix_mp.width &&
			    fmt.fmt.pix_mp.height ==
				    d->cap_fmt.fmt.pix_mp.height)
				continue;

			/* Re-read G_FMT inside and renegotiate CAPTURE. */
			d->width = fmt.fmt.pix_mp.width;
			d->height = fmt.fmt.pix_mp.height;
			ret = v4l2_dec_setup_capture(d);
			if (ret)
				return ret;
			*changed = 1;
		}
	}
	return 0;
}

/* Dequeue one CAPTURE frame; returns 0, 1 at EOS, -EAGAIN if none ready. */
int v4l2_dec_dqcap(struct v4l2_dec *d, struct v4l2_dec_frame *frame)
{
	struct v4l2_buffer b;
	struct v4l2_plane plane;

	memset(&b, 0, sizeof(b));
	memset(&plane, 0, sizeof(plane));
	b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	b.memory = d->cap_memory;
	b.length = 1;
	b.m.planes = &plane;
	if (xioctl(d->fd, VIDIOC_DQBUF, &b) < 0) {
		if (errno == EAGAIN)
			return -EAGAIN;
		perror("DQBUF CAPTURE");
		return -errno;
	}

	frame->index = b.index;
	frame->bytesused = b.m.planes[0].bytesused;
	frame->width = d->width;
	frame->height = d->height;
	frame->mem = d->cap_memory == V4L2_MEMORY_MMAP ?
		d->cap_mem[b.index] : NULL;
	frame->flags = b.flags;
	frame->timestamp = b.timestamp.tv_sec * 1000000000ULL +
			   b.timestamp.tv_usec * 1000ULL;
	frame->capture_generation = d->cap_generation;
	d->cap_queued[b.index] = 0;
	if (b.flags & V4L2_BUF_FLAG_LAST)
		d->eos = 1;

	return d->eos ? 1 : 0;
}

int v4l2_dec_qcap_idx(struct v4l2_dec *d, unsigned int index)
{
	int ret;

	if (index >= d->cap_count)
		return -EINVAL;
	if (d->cap_queued[index])
		return 0;
	memset(&d->cap_meta[index].m.planes[0], 0,
	       sizeof(d->cap_meta[index].m.planes[0]));
	if (d->cap_memory == V4L2_MEMORY_DMABUF) {
		d->cap_meta[index].m.planes[0].m.fd = d->cap_dmabuf_fds[index];
		d->cap_meta[index].m.planes[0].length = d->cap_dmabuf_sizes[index];
	}
	ret = xioctl(d->fd, VIDIOC_QBUF, &d->cap_meta[index]);
	if (ret < 0)
		return -errno;
	d->cap_queued[index] = 1;
	return 0;
}

int v4l2_dec_qcap(struct v4l2_dec *d, const struct v4l2_dec_frame *frame)
{
	return v4l2_dec_qcap_idx(d, frame->index);
}

int v4l2_dec_export(struct v4l2_dec *d, unsigned int cap_index, int *fd,
		    unsigned int *pitch, unsigned int *size)
{
	struct v4l2_exportbuffer exp;
	struct v4l2_plane *pl = &d->cap_meta[cap_index].m.planes[0];

	memset(&exp, 0, sizeof(exp));
	if (d->cap_memory == V4L2_MEMORY_DMABUF) {
		*fd = fcntl(d->cap_dmabuf_fds[cap_index], F_DUPFD_CLOEXEC, 0);
		if (*fd < 0)
			return -errno;
		*pitch = d->cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
		*size = d->cap_dmabuf_sizes[cap_index];
		return 0;
	}
	exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	exp.index = cap_index;
	if (xioctl(d->fd, VIDIOC_EXPBUF, &exp) < 0)
		return -errno;
	*fd = exp.fd;
	*pitch = d->cap_fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
	*size = d->cap_size[cap_index];
	(void)pl;
	return 0;
}

void v4l2_dec_size(struct v4l2_dec *d, unsigned int *w, unsigned int *h)
{
	*w = d->width;
	*h = d->height;
}

/* Recycle a finished OUTPUT buffer so it can be reused. Returns 0 on success,
 * -EAGAIN if no OUTPUT buffer is ready yet. */
int v4l2_dec_dqout(struct v4l2_dec *d)
{
	struct v4l2_buffer b;
	struct v4l2_plane plane;

	memset(&b, 0, sizeof(b));
	memset(&plane, 0, sizeof(plane));
	b.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	b.memory = V4L2_MEMORY_MMAP;
	b.length = 1;
	b.m.planes = &plane;
	if (xioctl(d->fd, VIDIOC_DQBUF, &b) < 0) {
		if (errno == EAGAIN)
			return -EAGAIN;
		return -errno;
	}
	d->free_out[d->free_out_n++] = b.index;
	return 0;
}

int v4l2_dec_flush(struct v4l2_dec *d)
{
	struct v4l2_decoder_cmd cmd;

	memset(&cmd, 0, sizeof(cmd));
	cmd.cmd = V4L2_DEC_CMD_STOP;
	return xioctl(d->fd, VIDIOC_DECODER_CMD, &cmd) < 0 ? -errno : 0;
}

int v4l2_dec_stop(struct v4l2_dec *d)
{
	enum v4l2_buf_type type;
	int ret = 0;

	/* A stateful decoder must stop accepting compressed input before its
	 * decoded-output buffers are torn down.  For qcom-iris this ordering is
	 * especially important: OUTPUT STREAMOFF issues HFI_FLUSH_ALL and moves
	 * the session out of IRIS_INST_STREAMING; stopping CAPTURE first only
	 * issues HFI_FLUSH_OUTPUT and can time out while 4K pictures are still in
	 * flight, leaving subsequent SESSION_INIT commands wedged. */
	if (d->streaming) {
		type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
		if (xioctl(d->fd, VIDIOC_STREAMOFF, &type) < 0) {
			ret = -errno;
			perror("STREAMOFF OUTPUT");
		}
		d->streaming = 0;
	}
	if (d->streaming_cap) {
		type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
		if (xioctl(d->fd, VIDIOC_STREAMOFF, &type) < 0) {
			if (!ret)
				ret = -errno;
			perror("STREAMOFF CAPTURE");
		}
		d->streaming_cap = 0;
	}
	return ret;
}

void v4l2_dec_close(struct v4l2_dec *d)
{
	unsigned int i;

	if (!d)
		return;
	/* IRIS_UNCLEAN_CLOSE=1: mimic SIGKILL teardown (no flush, no
	 * STREAMOFF, bare close()) for firmware-wedge repro work. */
	if (getenv("IRIS_UNCLEAN_CLOSE")) {
		printf("v4l2-dec: UNCLEAN close (no flush/stop)\n");
		if (d->fd >= 0)
			close(d->fd);
		d->fd = -1;
		return;
	}
	/* Politely drain before tearing the queues down.  A stateful session
	 * killed mid-flight (client error path, context destroyed between
	 * pictures) is what wedges the firmware into SESSION_INIT timeouts
	 * until the module is reloaded; V4L2_DEC_CMD_STOP + FLAG_LAST lets
	 * the firmware finish its current picture first. */
	if (d->streaming && !d->eos && v4l2_dec_flush(d) == 0) {
		int spin;

		for (spin = 0; spin < 25; spin++) {
			struct v4l2_dec_frame frame;
			int r;

			while (v4l2_dec_dqout(d) == 0)
				;
			r = v4l2_dec_dqcap(d, &frame);
			if (r == 1)
				break;	/* FLAG_LAST seen */
			if (!r) {
				if (v4l2_dec_qcap(d, &frame) < 0)
					break;
			} else if (r == -EAGAIN) {
				v4l2_dec_poll_cap(d, 10);
			} else if (r < 0) {
				break;
			}
		}
	}
	v4l2_dec_stop(d);
	if (d->cap_meta) {
		for (i = 0; i < d->cap_count; i++) {
			if (d->cap_mem && d->cap_mem[i])
				munmap(d->cap_mem[i], d->cap_size[i]);
			free(d->cap_meta[i].m.planes);
		}
		free(d->cap_mem);
		free(d->cap_size);
		free(d->cap_queued);
		free(d->cap_meta);
	}
	if (d->out_meta) {
		for (i = 0; i < d->out_count; i++) {
			if (d->out_mem && d->out_mem[i])
				munmap(d->out_mem[i], d->out_size[i]);
			free(d->out_meta[i].m.planes);
		}
		free(d->out_mem);
		free(d->out_size);
		free(d->out_meta);
		free(d->free_out);
	}
	free(d->cap_dmabuf_fds);
	free(d->cap_dmabuf_sizes);
	if (d->fd >= 0)
		close(d->fd);
}
