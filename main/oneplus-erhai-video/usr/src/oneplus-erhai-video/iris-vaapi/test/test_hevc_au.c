// SPDX-License-Identifier: GPL-2.0-or-later
/* Feed HEVC picture access units through the iris V4L2 decoder. */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "v4l2_dec.h"

struct au { const unsigned char *data; size_t len; };

static long find_start(const unsigned char *p, size_t n, size_t from)
{
	size_t i;
	for (i = from; i + 3 < n; i++) {
		if (p[i] == 0 && p[i + 1] == 0 && p[i + 2] == 1) return i;
		if (i + 4 <= n && p[i] == 0 && p[i + 1] == 0 &&
		    p[i + 2] == 0 && p[i + 3] == 1) return i;
	}
	return -1;
}

static size_t prefix_len(const unsigned char *p, size_t n, size_t pos)
{
	return pos + 4 <= n && p[pos + 2] == 0 ? 4 : 3;
}

static int split_aus(const unsigned char *stream, size_t size,
			    struct au **out, unsigned int *count)
{
	long pos, next, au_start = -1;
	int seen_vcl = 0;
	unsigned int n = 0;
	struct au *aus = NULL;

	pos = find_start(stream, size, 0);
	while (pos >= 0) {
		size_t h = (size_t)pos + prefix_len(stream, size, pos);
		int type, vcl, first;

		next = find_start(stream, size, h);
		if (h + 2 > size) break;
		type = (stream[h] >> 1) & 0x3f;
		vcl = type <= 31;
		first = vcl && h + 2 < size && (stream[h + 2] & 0x80);
		if (au_start < 0) au_start = pos;
		else if (vcl && first && seen_vcl) {
			aus = realloc(aus, (n + 1) * sizeof(*aus));
			if (!aus) return -ENOMEM;
			aus[n++] = (struct au){ stream + au_start,
				(size_t)pos - (size_t)au_start };
			au_start = pos;
			seen_vcl = 0;
		}
		if (vcl) seen_vcl = 1;
		pos = next;
	}
	if (au_start >= 0) {
		aus = realloc(aus, (n + 1) * sizeof(*aus));
		if (!aus) return -ENOMEM;
		aus[n++] = (struct au){ stream + au_start, size - (size_t)au_start };
	}
	*out = aus;
	*count = n;
	return 0;
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "/tmp/test-hevc.h265";
	unsigned int width = argc > 2 ? atoi(argv[2]) : 1920;
	unsigned int height = argc > 3 ? atoi(argv[3]) : 1088;
	unsigned int cap_pixfmt = argc > 4 && !strcmp(argv[4], "p010") ?
		V4L2_PIX_FMT_P010 : V4L2_PIX_FMT_NV12;
	unsigned char *stream;
	struct au *aus;
	struct v4l2_dec dec;
	struct v4l2_dec_frame frame;
	FILE *fp;
	long size;
	unsigned int n_aus, fed = 0, recycled = 0, frames = 0, corrupt = 0;
	struct timespec t0, t1;
	int ret, changed;

	fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	fseek(fp, 0, SEEK_END); size = ftell(fp); fseek(fp, 0, SEEK_SET);
	stream = malloc(size);
	if (!stream || fread(stream, 1, size, fp) != (size_t)size) return 1;
	fclose(fp);
	ret = split_aus(stream, size, &aus, &n_aus);
	if (ret || !n_aus) { fprintf(stderr, "no HEVC access units\n"); return 1; }
	printf("access units found: %u\n", n_aus);

	ret = v4l2_dec_open(&dec, "/dev/video0", width, height,
			    V4L2_PIX_FMT_HEVC, cap_pixfmt);
	if (ret) { fprintf(stderr, "open %d\n", ret); return 1; }
	ret = v4l2_dec_feed(&dec, aus[0].data, aus[0].len, 0);
	if (ret) { fprintf(stderr, "feed 0 %d\n", ret); return 1; }
	fed = 1;
	ret = v4l2_dec_start(&dec);
	if (ret) { fprintf(stderr, "start %d\n", ret); return 1; }
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (fed < n_aus || recycled < fed) {
		while (fed < n_aus && fed - recycled < 4) {
			ret = v4l2_dec_feed(&dec, aus[fed].data, aus[fed].len, fed);
			if (ret) { fprintf(stderr, "feed %u %d\n", fed, ret); goto done; }
			fed++;
		}
		ret = v4l2_dec_poll(&dec, 3000);
		if (ret <= 0) { fprintf(stderr, "poll %d errno=%d\n", ret, errno); goto done; }
		while (v4l2_dec_dqout(&dec) == 0) recycled++;
		ret = v4l2_dec_handle_events(&dec, &changed);
		if (ret) { fprintf(stderr, "events %d\n", ret); goto done; }
		for (;;) {
			ret = v4l2_dec_dqcap(&dec, &frame);
			if (ret == -EAGAIN) break;
			if (ret < 0) { fprintf(stderr, "dqcap %d\n", ret); goto done; }
			if (frame.bytesused &&
			    frame.flags & V4L2_BUF_FLAG_ERROR) {
				fprintf(stderr, "frame %u corrupt (flags=0x%x)\n",
					frames, frame.flags);
				corrupt++;
			}
			if (frame.bytesused) {
				frames++;
				v4l2_dec_qcap(&dec, &frame);
			}
			if (ret == 1) goto done;
		}
	}
	if (v4l2_dec_flush(&dec)) fprintf(stderr, "flush failed\n");
	for (;;) {
		ret = v4l2_dec_poll(&dec, 3000);
		if (ret <= 0) break;
		v4l2_dec_dqout(&dec);
		v4l2_dec_handle_events(&dec, &changed);
		ret = v4l2_dec_dqcap(&dec, &frame);
		if (ret == -EAGAIN) continue;
		if (ret < 0) break;
		if (frame.bytesused && frame.flags & V4L2_BUF_FLAG_ERROR) {
			fprintf(stderr, "frame %u corrupt (flags=0x%x)\n",
				frames, frame.flags);
			corrupt++;
		}
		if (frame.bytesused) {
			frames++;
			v4l2_dec_qcap(&dec, &frame);
		}
		if (ret == 1) break;
	}
done:
	clock_gettime(CLOCK_MONOTONIC, &t1);
	{
		double sec = t1.tv_sec - t0.tv_sec +
			(t1.tv_nsec - t0.tv_nsec) / 1e9;
		printf("decoded %u frames (%u corrupt), %.1f fps\n", frames,
		       corrupt,
		       sec > 0 ? frames / sec : 0);
	}
	v4l2_dec_close(&dec); free(aus); free(stream);
	return frames && !corrupt ? 0 : 1;
}
