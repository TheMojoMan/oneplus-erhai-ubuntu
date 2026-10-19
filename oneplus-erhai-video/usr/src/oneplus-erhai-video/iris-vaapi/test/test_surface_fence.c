// SPDX-License-Identifier: GPL-2.0-or-later
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "iris_surface_fence.h"

int main(int argc, char **argv)
{
	const char *video_path = argc > 1 ? argv[1] : "/dev/video0";
	struct dma_heap_allocation_data alloc = {
		.len = 4096,
		.fd_flags = O_RDWR | O_CLOEXEC,
	};
	struct iris_surface_fence_cmd fence = {
		.op = IRIS_SURFACE_FENCE_ATTACH,
		.token = 1,
	};
	struct dma_buf_export_sync_file export = {
		.flags = DMA_BUF_SYNC_READ,
		.fd = -1,
	};
	struct pollfd pollfd;
	int heap_fd, video_fd, ret = 1;

	heap_fd = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	if (heap_fd < 0) {
		perror("open dma_heap/system");
		return 1;
	}
	if (ioctl(heap_fd, DMA_HEAP_IOCTL_ALLOC, &alloc) < 0) {
		perror("DMA_HEAP_IOCTL_ALLOC");
		goto close_heap;
	}
	video_fd = open(video_path, O_RDWR | O_CLOEXEC);
	if (video_fd < 0) {
		perror("open Iris decoder");
		goto close_dmabuf;
	}

	fence.dmabuf_fd = alloc.fd;
	if (ioctl(video_fd, VIDIOC_IRIS_SURFACE_FENCE, &fence) < 0) {
		fprintf(stderr, "attach fence: %s (is the new qcom-iris.ko loaded?)\n",
			strerror(errno));
		goto close_video;
	}
	if (ioctl(alloc.fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &export) < 0) {
		perror("DMA_BUF_IOCTL_EXPORT_SYNC_FILE");
		goto signal_fence;
	}

	pollfd.fd = export.fd;
	pollfd.events = POLLIN;
	pollfd.revents = 0;
	if (poll(&pollfd, 1, 0) != 0) {
		fprintf(stderr, "fence was signaled before the surface write\n");
		goto close_sync_file;
	}

	fence.op = IRIS_SURFACE_FENCE_SIGNAL;
	fence.dmabuf_fd = -1;
	if (ioctl(video_fd, VIDIOC_IRIS_SURFACE_FENCE, &fence) < 0) {
		perror("signal fence");
		goto close_sync_file;
	}
	pollfd.revents = 0;
	if (poll(&pollfd, 1, 1000) != 1 || !(pollfd.revents & POLLIN)) {
		fprintf(stderr, "fence did not signal within one second\n");
		goto close_sync_file;
	}

	printf("surface fence attach/wait/signal: PASS (%s)\n", video_path);
	ret = 0;
	goto close_sync_file;

signal_fence:
	fence.op = IRIS_SURFACE_FENCE_SIGNAL;
	fence.dmabuf_fd = -1;
	ioctl(video_fd, VIDIOC_IRIS_SURFACE_FENCE, &fence);
close_sync_file:
	if (export.fd >= 0)
		close(export.fd);
close_video:
	close(video_fd);
close_dmabuf:
	close(alloc.fd);
close_heap:
	close(heap_fd);
	return ret;
}
