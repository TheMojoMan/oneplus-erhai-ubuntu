// SPDX-License-Identifier: GPL-2.0-or-later
/* P1 engine test: split an Annex-B H.264 stream into access units, feed them
 * through the iris V4L2 decoder, and verify NV12 frames come out.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "v4l2_dec.h"

/* ---- Annex-B parsing helpers ---- */

struct bsr {
	const unsigned char *p;
	const unsigned char *end;
	unsigned int bit;
};

static unsigned int bsr_read(struct bsr *b, int n)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < n; i++) {
		v = (v << 1) | ((b->p[0] >> (7 - b->bit)) & 1);
		if (++b->bit == 8) {
			b->bit = 0;
			b->p++;
			if (b->p >= b->end)
				break;
		}
	}
	return v;
}

static unsigned int bsr_ue(struct bsr *b)
{
	unsigned int zero = 0, val;

	while (!bsr_read(b, 1))
		zero++;
	val = (1U << zero) - 1;
	while (zero--)
		val += bsr_read(b, 1) << zero;
	return val;
}

static long find_start(const unsigned char *buf, size_t size, size_t from)
{
	size_t i;

	for (i = from; i + 3 < size; i++) {
		if (i + 4 < size && buf[i] == 0 && buf[i + 1] == 0 &&
		    buf[i + 2] == 0 && buf[i + 3] == 1)
			return i;
		/* Do not rediscover bytes 1..3 of a four-byte start code as a
		 * second three-byte start code. */
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 &&
		    (i == 0 || buf[i - 1] != 0))
			return i;
	}
	return -1;
}

static int nal_type(const unsigned char *p, size_t nalo)
{
	return p[nalo] & 0x1f;
}

static int first_mb(const unsigned char *p, size_t size, size_t nalo)
{
	struct bsr b;

	/* nalo is the 1-byte NAL header; slice payload begins at nalo+1. */
	if (nalo + 1 >= size)
		return -1;

	b.p = p + nalo + 1;
	b.end = p + size;
	b.bit = 0;

	return (int)bsr_ue(&b);
}

struct au {
	const unsigned char *data;
	size_t len;
};

/* ---- main ---- */

int main(int argc, char **argv)
{
	const char *path;
	unsigned int width = argc > 2 ? atoi(argv[2]) : 3840;
	unsigned int height = argc > 3 ? atoi(argv[3]) : 2160;
	struct au *aus = NULL;
	unsigned int n_aus = 0;
	unsigned char *stream;
	long fsize;
	FILE *fp;
	struct v4l2_dec dec;
	struct v4l2_dec_frame frame;
	FILE *dump = argc > 4 ? fopen(argv[4], "wb") : NULL;
	long pos, next;
	unsigned int fed = 0, recycled = 0, frames = 0;
	struct timespec t0, t1;
	double secs;
	int ret;

	if (argc < 2) {
		fprintf(stderr, "usage: %s input.h264 [width height [frame.nv12]]\n",
			argv[0]);
		return 2;
	}
	path = argv[1];

	fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	stream = malloc(fsize);
	if (!stream || fread(stream, 1, fsize, fp) != (size_t)fsize) {
		fprintf(stderr, "read failed\n");
		return 1;
	}
	fclose(fp);

	/* Split into access units.  An access unit starts at the first NAL after
	 * the previous picture's last slice: either a header NAL (AUD/SEI/
	 * SPS/PPS) or a VCL slice with first_mb_in_slice == 0. */
	{
		long au_start = -1;
		int seen_vcl = 0;

		pos = find_start(stream, fsize, 0);
		while (pos >= 0) {
			next = find_start(stream, fsize, pos + 1);
			{
				size_t nalo = pos + (stream[pos + 2] == 1 ? 3 : 4);
				int t = nal_type(stream, nalo);
				int vcl = (t >= 1 && t <= 5);
				int fm = vcl ? first_mb(stream, fsize, nalo) : -1;

				if (vcl) {
					if (fm == 0 && seen_vcl && au_start >= 0) {
						/* new picture: close previous AU */
						aus = realloc(aus, (n_aus + 1) * sizeof(*aus));
						if (!aus)
							return 1;
						aus[n_aus].data = stream + au_start;
						aus[n_aus].len = (size_t)(pos - au_start);
						n_aus++;
						au_start = pos;
					} else if (au_start < 0) {
						au_start = pos;
					}
					seen_vcl = 1;
				} else {
					if (seen_vcl && au_start >= 0) {
						/* header after a picture starts a new AU */
						aus = realloc(aus, (n_aus + 1) * sizeof(*aus));
						if (!aus)
							return 1;
						aus[n_aus].data = stream + au_start;
						aus[n_aus].len = (size_t)(pos - au_start);
						n_aus++;
						au_start = pos;
						seen_vcl = 0;
					} else if (au_start < 0) {
						au_start = pos;
					}
				}
			}
			pos = next;
		}
		if (au_start >= 0) {
			aus = realloc(aus, (n_aus + 1) * sizeof(*aus));
			if (!aus)
				return 1;
			aus[n_aus].data = stream + au_start;
			aus[n_aus].len = (size_t)(fsize - au_start);
			n_aus++;
		}
	}

	printf("access units found: %u\n", n_aus);

	ret = v4l2_dec_open(&dec, "/dev/video0", width, height,
			    V4L2_PIX_FMT_H264, V4L2_PIX_FMT_NV12);
	if (ret) { fprintf(stderr, "open failed ret=%d\n", ret); return 1; }

	/* Queue the first access unit before STREAMON, mirroring FFmpeg. */
	ret = v4l2_dec_feed(&dec, aus[0].data, aus[0].len, 0);
	if (ret) { fprintf(stderr, "feed au 0 failed ret=%d\n", ret); return 1; }
	fed = 1;

	ret = v4l2_dec_start(&dec);
	if (ret) { fprintf(stderr, "start failed ret=%d\n", ret); return 1; }

	clock_gettime(CLOCK_MONOTONIC, &t0);

	/* Feed AUs and drain frames with a poll-driven loop. */
	while (fed < n_aus || recycled < fed) {
		int changed;

		while (fed < n_aus && (int)(fed - recycled) < 4) {
			ret = v4l2_dec_feed(&dec, aus[fed].data, aus[fed].len, fed);
			if (ret) {
				fprintf(stderr, "feed au %u failed ret=%d\n",
					fed, ret);
				return 1;
			}
			fed++;
		}

		ret = v4l2_dec_poll(&dec, 3000);
		if (ret == 0) {
			fprintf(stderr, "poll timeout (fed=%u recycled=%u frames=%u)\n",
				fed, recycled, frames);
			break;
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

		{
			if (v4l2_dec_dqout(&dec) == 0)
				recycled++;
		}

		for (;;) {
			ret = v4l2_dec_dqcap(&dec, &frame);
			if (ret < 0)
				break;
			frames++;
			if (frames <= 3)
				printf("frame %u: %ux%u bytes=%u\n",
				       frames, frame.width, frame.height,
				       frame.bytesused);
			if (dump && frames == 1)
				fwrite(frame.mem, 1, frame.bytesused, dump);
			v4l2_dec_qcap(&dec, &frame);
			if (ret == 1)
				break;
		}
	}

	/* Stateful decoders may retain the final picture until STOP/EOS. */
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
				if (dump && frames == 1)
					fwrite(frame.mem, 1, frame.bytesused, dump);
			}
			if (ret == 1)
				goto drained;
			v4l2_dec_qcap(&dec, &frame);
		}
	}

drained:

	clock_gettime(CLOCK_MONOTONIC, &t1);
	secs = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	printf("access units: %u, decoded frames: %u\n", n_aus, frames);
	printf("decode rate: %.1f fps (%.3f s for %u frames)\n",
	       secs > 0 ? frames / secs : 0, secs, frames);

	if (dump)
		fclose(dump);
	v4l2_dec_close(&dec);
	free(stream);
	free(aus);
	return frames ? 0 : 1;
}
