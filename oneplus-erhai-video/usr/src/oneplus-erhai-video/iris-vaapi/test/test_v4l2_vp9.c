// SPDX-License-Identifier: GPL-2.0-or-later
/* Feed VP9 frames from an IVF file directly to the Iris V4L2 decoder. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "v4l2_dec.h"

struct frame { const unsigned char *data; size_t len; };

static unsigned int get_le32(const unsigned char *p)
{
	return (unsigned int)p[0] | (unsigned int)p[1] << 8 |
	       (unsigned int)p[2] << 16 | (unsigned int)p[3] << 24;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/test-vp9.ivf";
	unsigned int width = argc > 2 ? atoi(argv[2]) : 1920;
	unsigned int height = argc > 3 ? atoi(argv[3]) : 1080;
	unsigned int cap_pixfmt = argc > 4 && !strcmp(argv[4], "p010") ?
		V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
	unsigned char *stream, *p, *end;
	struct frame *frames = NULL;
	struct v4l2_dec dec;
	struct v4l2_dec_frame decoded;
	struct timespec t0, t1;
	unsigned int count = 0, fed = 0, recycled = 0, done = 0, corrupt = 0;
	FILE *fp;
	long size;
	int ret, changed;

	if (argc < 2) {
		fprintf(stderr, "usage: %s input.ivf [width height [p010]]\n", argv[0]);
		return 2;
	}
	fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	fseek(fp, 0, SEEK_END); size = ftell(fp); fseek(fp, 0, SEEK_SET);
	stream = malloc(size);
	if (!stream || fread(stream, 1, size, fp) != (size_t)size) return 1;
	fclose(fp);
	if (size < 32 || memcmp(stream, "DKIF", 4)) return 1;
	p = stream + 32;
	end = stream + size;
	while (p + 12 <= end) {
		unsigned int len = get_le32(p);

		if (len > 16 * 1024 * 1024 || p + 12 + len > end) break;
		frames = realloc(frames, (count + 1) * sizeof(*frames));
		if (!frames) return 1;
		frames[count++] = (struct frame){ p + 12, len };
		p += 12 + len;
	}
	if (!count) return 1;
	printf("IVF frames found: %u\n", count);

	ret = v4l2_dec_open(&dec, "/dev/video0", width, height,
			    V4L2_PIX_FMT_VP9, cap_pixfmt);
	if (ret) { fprintf(stderr, "open %d\n", ret); return 1; }
	ret = v4l2_dec_feed(&dec, frames[0].data, frames[0].len,
			    1000ULL * 1000000000ULL);
	if (ret) { fprintf(stderr, "feed 0 %d\n", ret); return 1; }
	fed = 1;
	ret = v4l2_dec_start(&dec);
	if (ret) { fprintf(stderr, "start %d\n", ret); return 1; }
	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (fed < count || recycled < fed) {
		while (fed < count && fed - recycled < 8) {
			ret = v4l2_dec_feed(&dec, frames[fed].data, frames[fed].len,
					    (fed + 1000ULL) * 1000000000ULL);
			if (ret) break;
			fed++;
		}
		ret = v4l2_dec_poll(&dec, 3000);
		if (ret <= 0) { fprintf(stderr, "poll %d errno=%d\n", ret, errno); break; }
		while (v4l2_dec_dqout(&dec) == 0) recycled++;
		ret = v4l2_dec_handle_events(&dec, &changed);
		if (ret) break;
		for (;;) {
			ret = v4l2_dec_dqcap(&dec, &decoded);
			if (ret < 0) break;
			if (decoded.bytesused) {
				done++;
				if (decoded.flags & V4L2_BUF_FLAG_ERROR) corrupt++;
				v4l2_dec_qcap(&dec, &decoded);
			}
		}
	}
	if (!v4l2_dec_flush(&dec)) {
		for (;;) {
			ret = v4l2_dec_poll_cap(&dec, 3000);
			if (ret <= 0) break;
			ret = v4l2_dec_dqcap(&dec, &decoded);
			if (ret == -EAGAIN) continue;
			if (ret < 0) break;
			if (decoded.bytesused) {
				done++;
				if (decoded.flags & V4L2_BUF_FLAG_ERROR) corrupt++;
				v4l2_dec_qcap(&dec, &decoded);
			}
			if (ret == 1) break;
		}
	}
	clock_gettime(CLOCK_MONOTONIC, &t1);
	{
		double sec = t1.tv_sec - t0.tv_sec +
			(t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("decoded %u frames (%u corrupt), %.1f fps\n",
		       done, corrupt, sec > 0 ? done / sec : 0);
	}
	v4l2_dec_close(&dec);
	free(frames);
	free(stream);
	return done && !corrupt ? 0 : 1;
}
