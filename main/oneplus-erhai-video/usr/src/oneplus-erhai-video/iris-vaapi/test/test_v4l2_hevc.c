// SPDX-License-Identifier: GPL-2.0-or-later
/* HEVC (Annex-B .265) through the iris V4L2 decoder; verify NV12/P010 frames
 * come out.  Usage: test_v4l2_hevc input.265 [width height [nv12|p010]] */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "v4l2_dec.h"
#include "dma_heap.h"

static long find_start(const unsigned char *b, size_t n, long from)
{
	for (long i = from; i + 3 < (long)n; i++)
		if (b[i] == 0 && b[i + 1] == 0 &&
		    (b[i + 2] == 1 || (b[i + 2] == 0 && i + 4 <= (long)n &&
				       b[i + 3] == 1)))
			return i;
	return -1;
}

/* HEVC NAL header: forbidden_zero(1) nal_type(6) layer(6+). VCL types 0..31. */
static int hevc_nal_type(const unsigned char *b, size_t off)
{
	return (b[off] >> 1) & 0x3f;
}

int main(int argc, char **argv)
{
	struct v4l2_dec dec = {0};
	struct v4l2_dec_frame frame;
	unsigned char *stream;
	struct timespec t0, t1;
	unsigned int width, height, capfmt;
	long pos, next;
	size_t fsize;
	unsigned char **au_data = NULL;
	size_t *au_len = NULL;
	unsigned n_aus = 0, fed = 0, recycled = 0, frames = 0;
	unsigned kill_after = 0;
	FILE *fp;
	int ret;
	const char *path;

	if (argc < 2) {
		fprintf(stderr,
			"usage: %s input.265 [width height [nv12|p010]]\n",
			argv[0]);
		return 1;
	}
	path = argv[1];
	width = argc > 2 ? (unsigned)atoi(argv[2]) : 3840;
	height = argc > 3 ? (unsigned)atoi(argv[3]) : 2160;
	capfmt = (argc > 4 && !strcmp(argv[4], "p010"))
			     ? V4L2_PIX_FMT_P010
			     : V4L2_PIX_FMT_NV12;
	{
		const char *ka = getenv("IRIS_KILL_AFTER");

		kill_after = ka ? (unsigned)atoi(ka) : 0;
	}

	fp = fopen(path, "rb");
	if (!fp) {
		perror(path);
		return 1;
	}
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	stream = malloc(fsize);
	if (!stream || fread(stream, 1, fsize, fp) != fsize) {
		fprintf(stderr, "read failed\n");
		return 1;
	}
	fclose(fp);

	/* Split into access units: an AU starts at a VCL NAL carrying the
	 * first_slice_segment_in_pic_flag, or at a parameter-set NAL that
	 * follows a picture. */
	{
		long au_start = -1;
		int seen_vcl = 0;

		pos = find_start(stream, fsize, 0);
		while (pos >= 0) {
			next = find_start(stream, fsize, pos + 1);
			{
				size_t nalo = pos +
					      (stream[pos + 2] == 1 ? 3 : 4);
				int t = hevc_nal_type(stream, nalo);
				int vcl = t <= 31;
				int first = vcl &&
					    nalo + 1 < fsize &&
					    (stream[nalo + 1] & 0x80);

				if (vcl && first && seen_vcl &&
				    au_start >= 0) {
					au_data = realloc(au_data,
							  (n_aus + 1) *
								  sizeof(*au_data));
					au_len = realloc(au_len,
							 (n_aus + 1) *
								 sizeof(*au_len));
					au_data[n_aus] = stream + au_start;
					au_len[n_aus] = (size_t)(pos -
								 au_start);
					n_aus++;
					au_start = pos;
				} else if (!vcl && seen_vcl && au_start >= 0) {
					au_data = realloc(au_data,
							  (n_aus + 1) *
								  sizeof(*au_data));
					au_len = realloc(au_len,
							 (n_aus + 1) *
								 sizeof(*au_len));
					au_data[n_aus] = stream + au_start;
					au_len[n_aus] = (size_t)(pos -
								 au_start);
					n_aus++;
					au_start = pos;
					seen_vcl = 0;
				} else if (au_start < 0) {
					au_start = pos;
				}
				if (vcl)
					seen_vcl = 1;
			}
			pos = next;
		}
		if (au_start >= 0 && au_start < (long)fsize) {
			au_data = realloc(au_data, (n_aus + 1) * sizeof(*au_data));
			au_len = realloc(au_len, (n_aus + 1) * sizeof(*au_len));
			au_data[n_aus] = stream + au_start;
			au_len[n_aus] = fsize - au_start;
			n_aus++;
		}
	}
	printf("access units found: %u\n", n_aus);
	if (!n_aus)
		return 1;

	ret = v4l2_dec_open(&dec, NULL, width, height, V4L2_PIX_FMT_HEVC,
			    capfmt);
	if (ret) {
		fprintf(stderr, "open failed ret=%d\n", ret);
		return 1;
	}
	if (getenv("IRIS_CAP_DMABUF")) {
		int fds[20];
		size_t sizes[20];
		size_t bufsz = (size_t)width * height * 3 + (1u << 20);
		unsigned int i;

		if (dma_heap_alloc(bufsz, 20, fds)) {
			fprintf(stderr, "dma-heap alloc failed\n");
			return 1;
		}
		for (i = 0; i < 20; i++) sizes[i] = bufsz;
		ret = v4l2_dec_set_capture_dmabufs(&dec, fds, sizes, 20);
		if (ret) {
			fprintf(stderr, "set_capture_dmabufs ret=%d\n", ret);
			return 1;
		}
		printf("CAPTURE import: 20 dmabufs of %zu bytes\n", bufsz);
	}

	ret = v4l2_dec_feed(&dec, au_data[0], au_len[0], 0);
	if (ret) {
		fprintf(stderr, "feed au 0 failed ret=%d\n", ret);
		return 1;
	}
	fed = 1;

	ret = v4l2_dec_start(&dec);
	if (ret) {
		fprintf(stderr, "start failed ret=%d\n", ret);
		return 1;
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (fed < n_aus || recycled < fed) {
		int changed;

		while (fed < n_aus && (int)(fed - recycled) < 4) {
			ret = v4l2_dec_feed(&dec, au_data[fed], au_len[fed],
					    fed);
			if (ret) {
				fprintf(stderr, "feed au %u failed ret=%d\n",
					fed, ret);
				return 1;
			}
			fed++;
		}

		ret = v4l2_dec_poll(&dec, 3000);
		if (ret == 0) {
			fprintf(stderr,
				"poll timeout (fed=%u recycled=%u frames=%u)\n",
				fed, recycled, frames);
			return 1;
		}
		if (ret < 0) {
			fprintf(stderr, "poll error %d\n", ret);
			return 1;
		}

		ret = v4l2_dec_handle_events(&dec, &changed);
		if (ret) {
			fprintf(stderr, "handle_events ret=%d\n", ret);
			return 1;
		}
		if (v4l2_dec_dqout(&dec) == 0)
			recycled++;

		for (;;) {
			ret = v4l2_dec_dqcap(&dec, &frame);
			if (ret < 0)
				break;
			frames++;
			if (kill_after && frames >= kill_after) {
				fprintf(stderr,
					"IRIS_KILL_AFTER: aborting at frame %u with decode in flight\n",
					frames);
				_exit(9);
			}
			if (frames <= 3)
				printf("frame %u: %ux%u bytes=%u\n", frames,
				       frame.width, frame.height,
				       frame.bytesused);
			v4l2_dec_qcap(&dec, &frame);
			if (ret == 1)
				break;
		}
	}

	ret = v4l2_dec_flush(&dec);
	if (ret) {
		fprintf(stderr, "flush failed ret=%d\n", ret);
		return 1;
	}
	for (;;) {
		int changed;

		ret = v4l2_dec_poll_cap(&dec, 3000);
		if (ret <= 0) {
			fprintf(stderr, "drain poll %s\n",
				ret ? "failed" : "timed out");
			return 1;
		}
		ret = v4l2_dec_handle_events(&dec, &changed);
		if (ret)
			return 1;
		for (;;) {
			ret = v4l2_dec_dqcap(&dec, &frame);
			if (ret == -EAGAIN)
				break;
			if (ret < 0)
				return 1;
			if (frame.bytesused) {
				frames++;
				if (frames <= 3)
					printf("drain frame %u: bytes=%u\n",
					       frames, frame.bytesused);
			}
			if (ret == 1)
				goto drained;
			v4l2_dec_qcap(&dec, &frame);
		}
	}
drained:
	v4l2_dec_close(&dec);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	printf("decoded %u frames of %u AUs in %.2fs\n", frames, n_aus,
	       (t1.tv_sec - t0.tv_sec) + 1e-9 * (t1.tv_nsec - t0.tv_nsec));
	return 0;
}
