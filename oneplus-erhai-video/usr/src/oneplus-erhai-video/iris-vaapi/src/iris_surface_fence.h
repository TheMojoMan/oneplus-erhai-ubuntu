// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef IRIS_SURFACE_FENCE_H
#define IRIS_SURFACE_FENCE_H

#include <linux/ioctl.h>
#include <linux/types.h>
#include <linux/videodev2.h>

struct iris_surface_fence_cmd {
	__u32 op;
	__s32 dmabuf_fd;
	__u64 token;
};

#define IRIS_SURFACE_FENCE_ATTACH	1
#define IRIS_SURFACE_FENCE_SIGNAL	2
#define VIDIOC_IRIS_SURFACE_FENCE \
	_IOW('V', BASE_VIDIOC_PRIVATE, struct iris_surface_fence_cmd)

#endif
