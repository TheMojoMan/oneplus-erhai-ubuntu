// SPDX-License-Identifier: GPL-2.0-or-later
/* Long-run VA-API decode stress: loops an Annex-B H.264 stream through the
 * iris driver well past 512 pictures to verify surface matching survives
 * sequence-number wraparound.  Mirrors Chrome's pipelined usage: keep a few
 * AUs in flight, sync each render target a few pictures behind.  Aborts on
 * the first failed vaSyncSurface, reporting the picture index.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

/* Pictures kept in flight before syncing an earlier one. */
#define LOOKAHEAD 10

/* ---- bit reader for SPS/PPS parsing (same as test_va_decode.c) ---- */

struct br {
	const uint8_t *p;
	const uint8_t *end;
	unsigned int bit;
};

static unsigned int br_bit(struct br *b)
{
	unsigned int v = (b->p[0] >> (7 - b->bit)) & 1;

	if (++b->bit == 8) {
		b->bit = 0;
		b->p++;
	}
	return v;
}

static unsigned int br_bits(struct br *b, int n)
{
	unsigned int v = 0;
	int i;

	for (i = 0; i < n; i++)
		v = (v << 1) | br_bit(b);
	return v;
}

static unsigned int br_ue(struct br *b)
{
	unsigned int z = 0, v;

	while (!br_bit(b))
		z++;
	v = (1u << z) - 1;
	while (z--)
		v += br_bit(b) << z;
	return v;
}

static int br_se(struct br *b)
{
	unsigned int u = br_ue(b);

	return (u & 1) ? (int)((u + 1) >> 1) : -(int)(u >> 1);
}

struct nal {
	const uint8_t *p;
	size_t len;
	int type;
};

static long find_start(const uint8_t *buf, size_t size, size_t from)
{
	size_t i;

	for (i = from; i + 3 < size; i++) {
		if (i + 4 < size && buf[i] == 0 && buf[i + 1] == 0 &&
		    buf[i + 2] == 0 && buf[i + 3] == 1)
			return i + 4;
		if (buf[i] == 0 && buf[i + 1] == 0 && buf[i + 2] == 1 &&
		    (i == from || buf[i - 1] != 0))
			return i + 3;
	}
	return -1;
}

static void parse_sps(const uint8_t *nal, size_t len,
		      VAPictureParameterBufferH264 *pic)
{
	struct br b;
	unsigned int u;

	memset(pic, 0, sizeof(*pic));
	b.p = nal + 1;
	b.end = nal + len;
	b.bit = 0;

	(void)br_bits(&b, 8);
	(void)br_bits(&b, 8);
	(void)br_bits(&b, 8);
	(void)br_ue(&b);

	u = br_ue(&b);
	pic->seq_fields.bits.chroma_format_idc = u;
	pic->bit_depth_luma_minus8 = br_ue(&b);
	pic->bit_depth_chroma_minus8 = br_ue(&b);
	(void)br_bit(&b);
	if (br_bit(&b))
		return;

	pic->seq_fields.bits.log2_max_frame_num_minus4 = br_ue(&b);
	u = br_ue(&b);
	pic->seq_fields.bits.pic_order_cnt_type = u;
	if (u == 0)
		pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 =
			br_ue(&b);

	pic->num_ref_frames = br_ue(&b);
	pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag = br_bit(&b);
	pic->picture_width_in_mbs_minus1 = br_ue(&b);
	pic->picture_height_in_mbs_minus1 = br_ue(&b);
	pic->seq_fields.bits.frame_mbs_only_flag = br_bit(&b);
	pic->seq_fields.bits.direct_8x8_inference_flag = br_bit(&b);
}

static void parse_pps(const uint8_t *nal, size_t len,
		      VAPictureParameterBufferH264 *pic)
{
	struct br b;

	b.p = nal + 1;
	b.end = nal + len;
	b.bit = 0;

	(void)br_ue(&b);
	(void)br_ue(&b);
	pic->pic_fields.bits.entropy_coding_mode_flag = br_bit(&b);
	pic->pic_fields.bits.pic_order_present_flag = br_bit(&b);
	(void)br_ue(&b);
	(void)br_ue(&b);
	(void)br_ue(&b);
	pic->pic_fields.bits.weighted_pred_flag = br_bit(&b);
	pic->pic_fields.bits.weighted_bipred_idc = br_bits(&b, 2);
	pic->pic_init_qp_minus26 = br_se(&b);
	pic->pic_init_qs_minus26 = br_se(&b);
	pic->chroma_qp_index_offset = br_se(&b);
	pic->pic_fields.bits.deblocking_filter_control_present_flag = br_bit(&b);
	pic->pic_fields.bits.constrained_intra_pred_flag = br_bit(&b);
	pic->pic_fields.bits.redundant_pic_cnt_present_flag = br_bit(&b);
	pic->pic_fields.bits.transform_8x8_mode_flag = br_bit(&b);
	(void)br_bit(&b);
	pic->second_chroma_qp_index_offset = br_se(&b);
}

static double now_s(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec + ts.tv_nsec / 1e9;
}

int main(int argc, char **argv)
{
	const char *path;
	long total = argc > 2 ? atol(argv[2]) : 700;
	uint8_t *data;
	long fsize;
	FILE *fp;
	long pos;
	static struct nal nals[4096];
	int nnals = 0;
	static int au_start[512], au_end[512];
	int naus = 0;
	VAPictureParameterBufferH264 pic;
	VADisplay dpy;
	VAConfigID cfg;
	VAContextID ctx;
	VASurfaceID surf[16];
	VABufferID sp_buf, pp_buf;
	VAStatus st;
	int major = 0, minor = 0;
	int fd, i, k;
	double t_feed = 0, t_sync = 0;
	int n_sync_ok = 0, worst_us = 0;
	double t0, t1;

	if (argc < 2) {
		fprintf(stderr, "usage: %s input.h264 [frame-count]\n", argv[0]);
		return 2;
	}
	path = argv[1];

	fp = fopen(path, "rb");
	if (!fp) { perror(path); return 1; }
	fseek(fp, 0, SEEK_END);
	fsize = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	data = malloc(fsize);
	if (!data || fread(data, 1, fsize, fp) != (size_t)fsize)
		return 1;
	fclose(fp);

	pos = find_start(data, fsize, 0);
	memset(&pic, 0, sizeof(pic));
	while (pos >= 0 && nnals < 4096) {
		long next = find_start(data, fsize, pos + 3);
		size_t end = next < 0 ? (size_t)fsize : (size_t)next;
		int t = data[pos] & 0x1f;

		nals[nnals].p = data + pos;
		nals[nnals].len = end - (size_t)pos;
		nals[nnals].type = t;
		if (t == 7)
			parse_sps(nals[nnals].p, nals[nnals].len, &pic);
		else if (t == 8)
			parse_pps(nals[nnals].p, nals[nnals].len, &pic);
		nnals++;
		pos = next;
	}

	/* Group NALs into access units.  A new picture starts at a VCL NAL
	 * whose first_mb_in_slice is 0; further VCL NALs join the current
	 * picture (multi-slice).  Non-VCL NALs seen since the previous AU are
	 * attached to the following one. */
	{
		int pending_nonvcl_from = -1;

		for (i = 0; i < nnals; i++) {
			int vcl = nals[i].type >= 1 && nals[i].type <= 5;
			unsigned int first_mb = 0;

			if (!vcl) {
				if (pending_nonvcl_from < 0)
					pending_nonvcl_from = i;
				continue;
			}
			{
				struct br b = { nals[i].p + 1,
						nals[i].p + nals[i].len, 0 };

				first_mb = br_ue(&b);
			}
			if (first_mb == 0 && naus < 512) {
				if (naus > 0 && au_end[naus - 1] == -1)
					au_end[naus - 1] = i;
				au_start[naus] = pending_nonvcl_from >= 0 ?
						 pending_nonvcl_from : i;
				au_end[naus] = -1;
				naus++;
			}
			pending_nonvcl_from = -1;
		}
		for (i = naus - 1; i >= 0; i--) {
			if (au_end[i] == -1) {
				au_end[i] = nnals;
				break;
			}
		}
	}
	printf("stream: %d NALs, %d AUs\n", nnals, naus);
	if (naus < 2) { printf("need more AUs\n"); return 1; }

	/* Optional: dump the assembled AU byte stream (Annex-B) for feeding
	 * an independent decoder / engine test. */
	if (getenv("IRIS_STRESS_DUMP")) {
		FILE *d = fopen(getenv("IRIS_STRESS_DUMP"), "wb");
		int a, j;
		static const uint8_t sc[4] = { 0, 0, 0, 1 };

		if (d) {
			for (a = 0; a < naus; a++)
				for (j = au_start[a]; j < au_end[a]; j++) {
					fwrite(sc, 1, 4, d);
					fwrite(nals[j].p, 1, nals[j].len, d);
				}
			fclose(d);
		}
	}
	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) { perror("open renderD128"); return 1; }
	dpy = vaGetDisplayDRM(fd);
	st = vaInitialize(dpy, &major, &minor);
	if (st != VA_STATUS_SUCCESS) {
		fprintf(stderr, "vaInitialize: %s\n", vaErrorStr(st));
		return 1;
	}
	st = vaCreateConfig(dpy, VAProfileH264High, VAEntrypointVLD, NULL, 0,
			    &cfg);
	if (st) { fprintf(stderr, "vaCreateConfig: %s\n", vaErrorStr(st)); return 1; }
	st = vaCreateContext(dpy, cfg, 1920, 1088, 0, NULL, 0, &ctx);
	if (st) { fprintf(stderr, "vaCreateContext: %s\n", vaErrorStr(st)); return 1; }
	st = vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, 1920, 1088, surf, 16,
			      NULL, 0);
	if (st) { fprintf(stderr, "vaCreateSurfaces: %s\n", vaErrorStr(st)); return 1; }
	/* Optional Chrome-style setup: export the pool before its first decode.
	 * The driver must retain its conservative visibility wait for these
	 * already-importable backings while leaving the default stress path
	 * asynchronous. */
	if (getenv("IRIS_STRESS_EXPORT")) {
		for (i = 0; i < 16; i++) {
			VADRMPRIMESurfaceDescriptor desc;

			memset(&desc, 0, sizeof(desc));
			st = vaExportSurfaceHandle(dpy, surf[i],
				VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
				VA_EXPORT_SURFACE_COMPOSED_LAYERS, &desc);
			if (st) {
				fprintf(stderr, "pre-export %d: %s\n", i,
					vaErrorStr(st));
				return 1;
			}
			close(desc.objects[0].fd);
		}
	}

	{
		VASliceParameterBufferH264 sp;

		memset(&sp, 0, sizeof(sp));
		/* Real value for this stream (test_va_decode reports g_nrl0=0):
		 * one reference per P frame.  Over-reporting makes the firmware
		 * reject every picture and nothing is ever output. */
		sp.num_ref_idx_l0_active_minus1 = 0;
		sp.num_ref_idx_l1_active_minus1 = 0;
		st = vaCreateBuffer(dpy, ctx, VASliceParameterBufferType,
				    sizeof(sp), 1, &sp, &sp_buf);
		if (st) { fprintf(stderr, "slice param buf: %s\n", vaErrorStr(st)); return 1; }
		st = vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType,
				    sizeof(pic), 1, &pic, &pp_buf);
		if (st) { fprintf(stderr, "pic param buf: %s\n", vaErrorStr(st)); return 1; }
	}

	for (i = 0; i < total; i++) {
		int a = i % naus;
		int j;
		VASurfaceID target = surf[i % 16];
		VABufferID rb[64];
		int nrb = 0;

		rb[nrb++] = pp_buf;
		rb[nrb++] = sp_buf;
		for (j = au_start[a]; j < au_end[a]; j++) {
			VABufferID b;

			st = vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
					    nals[j].len, 1, (void *)nals[j].p,
					    &b);
			if (st) { fprintf(stderr, "slice buf: %s\n", vaErrorStr(st)); return 1; }
			rb[nrb++] = b;
			if (nrb >= 63)
				break;
		}

		t0 = now_s();
		st = vaBeginPicture(dpy, ctx, target);
		if (st) { fprintf(stderr, "begin %d: %s\n", i, vaErrorStr(st)); return 1; }
		st = vaRenderPicture(dpy, ctx, rb, nrb);
		if (st) { fprintf(stderr, "render %d: %s\n", i, vaErrorStr(st)); return 1; }
		st = vaEndPicture(dpy, ctx);
		if (st) { fprintf(stderr, "end %d: %s\n", i, vaErrorStr(st)); return 1; }
		t1 = now_s();
		t_feed += t1 - t0;

		for (j = 2; j < nrb; j++)
			vaDestroyBuffer(dpy, rb[j]);

		/* Chrome-style pipelining: sync the picture fed LOOKAHEAD AUs
		 * ago; its frame is long since released. */
		if (i >= LOOKAHEAD) {
			VASurfaceID old = surf[(i - LOOKAHEAD) % 16];

			t0 = now_s();
			st = vaSyncSurface(dpy, old);
			t1 = now_s();
			t_sync += t1 - t0;
			{
				int us = (int)((t1 - t0) * 1e6);

				if (us > worst_us)
					worst_us = us;
			}
			if (st) {
				fprintf(stderr,
					"FAIL: sync pic %d (surf %u): %s "
					"after %.2fs\n",
					i - LOOKAHEAD, old, vaErrorStr(st),
					t1 - t0);
				return 1;
			}
			n_sync_ok++;
		}

		if ((i % 100) == 0)
			printf("[%4d] fed ok, sync ok=%d feed=%.2fs sync=%.2fs worst=%dus\n",
			       i, n_sync_ok, t_feed, t_sync, worst_us);
	}

	/* Drain the tail: the last LOOKAHEAD pictures.  Syncing the final one
	 * makes the driver issue the EOS flush that releases the firmware-held
	 * frame; the ones before it are already out by then. */
	for (k = (total > LOOKAHEAD ? total - LOOKAHEAD : 0); k < total; k++) {
		VASurfaceID tail = surf[k % 16];

		st = vaSyncSurface(dpy, tail);
		if (st) {
			fprintf(stderr, "FAIL: tail sync pic %d: %s\n", k,
				vaErrorStr(st));
			return 1;
		}
		n_sync_ok++;
	}

	printf("STRESS OK: fed %ld, synced %d, feed=%.2fs sync=%.2fs "
	       "worst_sync=%dus\n", total, n_sync_ok, t_feed, t_sync,
	       worst_us);
	free(data);
	return 0;
}
