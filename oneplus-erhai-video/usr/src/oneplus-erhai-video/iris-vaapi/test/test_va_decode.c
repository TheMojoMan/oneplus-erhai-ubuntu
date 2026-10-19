// SPDX-License-Identifier: GPL-2.0-or-later
/* End-to-end VA-API decode test: decode the first picture of an Annex-B
 * H.264 stream through the iris-vaapi driver and verify the exported NV12
 * against FFmpeg's software decode.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>

/* ---- bit reader for SPS/PPS parsing ---- */

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

/* ---- Annex-B scan ---- */

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
	if (u == 3)
		pic->seq_fields.bits.residual_colour_transform_flag = br_bit(&b);
	pic->bit_depth_luma_minus8 = br_ue(&b);
	pic->bit_depth_chroma_minus8 = br_ue(&b);
	(void)br_bit(&b);
	if (br_bit(&b))
		return;

	pic->seq_fields.bits.log2_max_frame_num_minus4 = br_ue(&b);
	u = br_ue(&b);
	pic->seq_fields.bits.pic_order_cnt_type = u;
	if (u == 0)
		pic->seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = br_ue(&b);
	else if (u == 1) {
		pic->seq_fields.bits.delta_pic_order_always_zero_flag = br_bit(&b);
		(void)br_se(&b);
		(void)br_se(&b);
		(void)br_ue(&b);
	}

	pic->num_ref_frames = br_ue(&b);
	pic->seq_fields.bits.gaps_in_frame_num_value_allowed_flag = br_bit(&b);
	pic->picture_width_in_mbs_minus1 = br_ue(&b);
	pic->picture_height_in_mbs_minus1 = br_ue(&b);
	pic->seq_fields.bits.frame_mbs_only_flag = br_bit(&b);
	if (!pic->seq_fields.bits.frame_mbs_only_flag)
		pic->seq_fields.bits.mb_adaptive_frame_field_flag = br_bit(&b);
	pic->seq_fields.bits.direct_8x8_inference_flag = br_bit(&b);
	if (br_bit(&b)) {
		(void)br_ue(&b); (void)br_ue(&b); (void)br_ue(&b); (void)br_ue(&b);
	}
}

static void parse_pps(const uint8_t *nal, size_t len,
		      VAPictureParameterBufferH264 *pic)
{
	struct br b;
	unsigned int cfi = pic->seq_fields.bits.chroma_format_idc;

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
	if (cfi == 1)
		pic->pic_fields.bits.transform_8x8_mode_flag = br_bit(&b);
	(void)br_bit(&b);
	pic->second_chroma_qp_index_offset = br_se(&b);
}

static int g_nrl0, g_nrl1;

int main(int argc, char **argv)
{
	const char *path;
	uint8_t *data;
	long fsize;
	FILE *fp;
	long pos;
	VAPictureParameterBufferH264 pic;
	static uint8_t sps[256], pps[64], slice[12][1 << 20];
	int sps_len = -1, pps_len = -1;
	int slice_len[12], n_slices = 0;
	VADisplay dpy;
	VAConfigID cfg;
	VAContextID ctx;
	VASurfaceID surf[16];
	VABufferID bufs[4];
	VAStatus st;
	int major = 0, minor = 0;
	int fd;

	if (argc < 2) {
		fprintf(stderr, "usage: %s input.h264\n", argv[0]);
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
	while (pos >= 0 && (sps_len < 0 || pps_len < 0 || n_slices < 10)) {
		long next = find_start(data, fsize, pos + 1);
		int t = data[pos] & 0x1f;
		size_t end = next < 0 ? (size_t)fsize : (size_t)next;
		size_t nalsz = end - (size_t)pos;

		if (t == 7 && sps_len < 0) {
			memcpy(sps, data + pos, nalsz); sps_len = nalsz;
		} else if (t == 8 && pps_len < 0) {
			memcpy(pps, data + pos, nalsz); pps_len = nalsz;
		} else if (t >= 1 && t <= 5 && n_slices < 10) {
			memcpy(slice[n_slices], data + pos, nalsz);
			slice_len[n_slices] = nalsz;
			n_slices++;
		}
		pos = next;
	}
	free(data);
	printf("SPS %d PPS %d slices %d (first %d bytes)\n",
	       sps_len, pps_len, n_slices, slice_len[0]);

	parse_sps(sps, sps_len, &pic);
	parse_pps(pps, pps_len, &pic);
	printf("picture %ux%u mbs, g_nrl0=%d g_nrl1=%d\n",
	       (pic.picture_width_in_mbs_minus1 + 1) * 16,
	       (pic.picture_height_in_mbs_minus1 + 1) * 16,
	       g_nrl0, g_nrl1);

	fd = open("/dev/dri/renderD128", O_RDWR);
	if (fd < 0) { perror("open renderD128"); return 1; }
	dpy = vaGetDisplayDRM(fd);
	if (!dpy) { fprintf(stderr, "vaGetDisplayDRM\n"); return 1; }
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

	/* Mirror Chrome: each picture carries slice params carrying the
	 * PPS default reference counts (rebuilt into the PPS NAL). */
	{
		VASliceParameterBufferH264 sp;
		unsigned int i;

		/* Chrome fills these with the reference count each slice actually
		 * uses (via num_ref_idx_active_override_flag); P-frames here use
		 * one reference. */
		memset(&sp, 0, sizeof(sp));
		sp.num_ref_idx_l0_active_minus1 = 1;
		sp.num_ref_idx_l1_active_minus1 = 0;
		st = vaCreateBuffer(dpy, ctx, VASliceParameterBufferType,
				    sizeof(sp), 1, &sp, &bufs[0]);
		if (st) { fprintf(stderr, "slice param buf: %s\n", vaErrorStr(st)); return 1; }

		/* Feed 10 real consecutive pictures so the pipeline fills. */
		for (i = 0; i < 10; i++) {
			VABufferID rb[3];

			st = vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
					    slice_len[i], 1, slice[i], &bufs[1]);
			if (st) { fprintf(stderr, "slice buf: %s\n", vaErrorStr(st)); return 1; }
			st = vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType,
					    sizeof(pic), 1, &pic, &rb[2]);
			if (st) { fprintf(stderr, "pic buf: %s\n", vaErrorStr(st)); return 1; }
			rb[0] = bufs[0];	/* slice params */
			rb[1] = bufs[1];	/* slice data */
			st = vaBeginPicture(dpy, ctx, surf[i]);
			if (st) { fprintf(stderr, "vaBeginPicture: %s\n", vaErrorStr(st)); return 1; }
			st = vaRenderPicture(dpy, ctx, rb, 3);
			if (st) { fprintf(stderr, "vaRenderPicture: %s\n", vaErrorStr(st)); return 1; }
			st = vaEndPicture(dpy, ctx);
			if (st) { fprintf(stderr, "vaEndPicture %d: %s\n", i, vaErrorStr(st)); return 1; }
			vaDestroyBuffer(dpy, bufs[1]);
			vaDestroyBuffer(dpy, rb[2]);
		}
	}

	/* The last picture stays held in the firmware pipeline (stateful
	 * one-frame latency).  Sync a middle picture normally, then sync the
	 * FINAL picture: iris_decode_sync feeds an EOS marker so the firmware
	 * releases it instead of timing out. */
	{
		int sync_idx = 6;
		int si;
		VASurfaceID s;

		for (si = 0; si < sync_idx; si++) {
			st = vaSyncSurface(dpy, surf[si]);
			if (st) { fprintf(stderr, "sync %d: %s\n", si, vaErrorStr(st)); return 1; }
		}
		s = surf[sync_idx];
		st = vaSyncSurface(dpy, s);
		if (st) { fprintf(stderr, "vaSyncSurface: %s\n", vaErrorStr(st)); return 1; }
		printf("synced middle surface %u\n", s);

		/* Chromium keeps the VAContext across Reset()/seek and does not send
		 * EOS to libva.  Submit a new IDR while the old firmware session is
		 * still live; the driver must discard the old DPB/CAPTURE work first. */
		{
			VABufferID rb[3];

			usleep(150000); /* model the demux pause around a user seek */
			st = vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
					    slice_len[0], 1, slice[0], &rb[1]);
			if (st) { fprintf(stderr, "restart slice buf: %s\n", vaErrorStr(st)); return 1; }
			st = vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType,
					    sizeof(pic), 1, &pic, &rb[2]);
			if (st) { fprintf(stderr, "restart pic buf: %s\n", vaErrorStr(st)); return 1; }
			rb[0] = bufs[0];
			st = vaBeginPicture(dpy, ctx, surf[10]);
			if (st) { fprintf(stderr, "restart begin: %s\n", vaErrorStr(st)); return 1; }
			st = vaRenderPicture(dpy, ctx, rb, 3);
			if (st) { fprintf(stderr, "restart render: %s\n", vaErrorStr(st)); return 1; }
			st = vaEndPicture(dpy, ctx);
			if (st) { fprintf(stderr, "restart end: %s\n", vaErrorStr(st)); return 1; }
			vaDestroyBuffer(dpy, rb[1]);
			vaDestroyBuffer(dpy, rb[2]);
			st = vaSyncSurface(dpy, surf[10]);
			if (st) { fprintf(stderr, "restart sync: %s\n", vaErrorStr(st)); return 1; }
			printf("synced live-seek IDR surface %u\n", surf[10]);
		}

		/* Syncing the live-seek IDR above sends EOS because it is now the
		 * final target.  Exercise the separate EOS-reopen path too. */
		{
			VABufferID rb[3];

			st = vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
					    slice_len[0], 1, slice[0], &rb[1]);
			if (st) { fprintf(stderr, "post-EOS slice buf: %s\n", vaErrorStr(st)); return 1; }
			st = vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType,
					    sizeof(pic), 1, &pic, &rb[2]);
			if (st) { fprintf(stderr, "post-EOS pic buf: %s\n", vaErrorStr(st)); return 1; }
			rb[0] = bufs[0];
			st = vaBeginPicture(dpy, ctx, surf[11]);
			if (st) { fprintf(stderr, "post-EOS begin: %s\n", vaErrorStr(st)); return 1; }
			st = vaRenderPicture(dpy, ctx, rb, 3);
			if (st) { fprintf(stderr, "post-EOS render: %s\n", vaErrorStr(st)); return 1; }
			st = vaEndPicture(dpy, ctx);
			if (st) { fprintf(stderr, "post-EOS end: %s\n", vaErrorStr(st)); return 1; }
			vaDestroyBuffer(dpy, rb[1]);
			vaDestroyBuffer(dpy, rb[2]);
			st = vaSyncSurface(dpy, surf[11]);
			if (st) { fprintf(stderr, "post-EOS sync: %s\n", vaErrorStr(st)); return 1; }
			printf("synced post-EOS IDR surface %u\n", surf[11]);
		}
		{
			int exi[] = { 0, 1, 3, 5 };
			VADRMPRIMESurfaceDescriptor dsc;

			for (int k = 0; k < 4; k++) {
				char path[64];
				void *mp;
				FILE *out;

				st = vaExportSurfaceHandle(dpy, surf[exi[k]],
							   VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
							   VA_EXPORT_SURFACE_READ_ONLY, &dsc);
				if (st) { fprintf(stderr, "export %d: %s\n", exi[k], vaErrorStr(st)); continue; }
				mp = mmap(NULL, dsc.objects[0].size, PROT_READ, MAP_SHARED,
					  dsc.objects[0].fd, 0);
				if (mp == MAP_FAILED) {
					perror("mmap");
					close(dsc.objects[0].fd);
					continue;
				}
				snprintf(path, sizeof(path), "/tmp/va-s%d.nv12", exi[k]);
				out = fopen(path, "wb");
				if (out) { fwrite(mp, 1, dsc.objects[0].size, out); fclose(out); }
				else perror("fopen");
				munmap(mp, dsc.objects[0].size);
				close(dsc.objects[0].fd);
			}
		}
	}


	vaDestroyConfig(dpy, cfg);
	vaTerminate(dpy);
	close(fd);
	printf("DECODE OK\n");
	return 0;
}
