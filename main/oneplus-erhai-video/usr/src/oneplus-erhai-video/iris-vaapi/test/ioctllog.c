// SPDX-License-Identifier: GPL-2.0-or-later
/* LD_PRELOAD ioctl logger for /dev/video0 — observe the exact V4L2 sequence
 * mpv uses so the iris-vaapi engine can replicate it. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <linux/videodev2.h>

static int (*real_ioctl)(int, unsigned long, ...) = NULL;
static int logfd = -1;

static int is_video_fd(int fd)
{
	struct stat st;

	if (fstat(fd, &st) < 0)
		return 0;
	return S_ISCHR(st.st_mode) &&
	       (strstr(st.st_rdev ? "/dev/video0" : "", "video") || 1);
}

static void plog(const char *fmt, ...)
{
	va_list ap;
	FILE *f = fopen("/tmp/ioctl.log", "a");

	if (!f)
		return;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fclose(f);
}

int ioctl(int fd, unsigned long req, ...)
{
	void *arg;
	va_list ap;
	int r;

	if (!real_ioctl)
		real_ioctl = dlsym(RTLD_NEXT, "ioctl");

	va_start(ap, req);
	arg = va_arg(ap, void *);
	va_end(ap);

	r = real_ioctl(fd, req, arg);

	/* log V4L2 ioctls (request codes 0x5600-0x56FF) */
	{
		if (((req >> 8) & 0xff) == 0x56) {
			const char *reqname = "?";
			switch (req & 0xff) {
			case _IOC_NR(VIDIOC_QUERYCAP): reqname = "QUERYCAP"; break;
			case _IOC_NR(VIDIOC_S_FMT):
			case _IOC_NR(VIDIOC_G_FMT): reqname = "S_G_FMT"; break;
			case _IOC_NR(VIDIOC_REQBUFS): reqname = "REQBUFS"; break;
			case _IOC_NR(VIDIOC_QUERYBUF): reqname = "QUERYBUF"; break;
			case _IOC_NR(VIDIOC_QBUF): reqname = "QBUF"; break;
			case _IOC_NR(VIDIOC_DQBUF): reqname = "DQBUF"; break;
			case _IOC_NR(VIDIOC_STREAMON): reqname = "STREAMON"; break;
			case _IOC_NR(VIDIOC_STREAMOFF): reqname = "STREAMOFF"; break;
			case _IOC_NR(VIDIOC_SUBSCRIBE_EVENT): reqname = "SUBSCRIBE_EVENT"; break;
			case _IOC_NR(VIDIOC_DQEVENT): reqname = "DQEVENT"; break;
			case _IOC_NR(VIDIOC_G_CROP): reqname = "G_CROP"; break;
			}
			plog("ioctl(fd=%d, %s) -> %d\n", fd, reqname, r);
		}
	}
	return r;
}
