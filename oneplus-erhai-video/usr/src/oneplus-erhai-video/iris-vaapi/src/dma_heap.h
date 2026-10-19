#ifndef IRIS_DMA_HEAP_H
#define IRIS_DMA_HEAP_H
/* Minimal /dev/dma_heap/system allocator for test harnesses that need to
 * mimic chromium's DMABUF CAPTURE import path. */
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/dma-heap.h>

static int dma_heap_alloc(size_t size, unsigned int count, int *fds)
{
	int heap = open("/dev/dma_heap/system", O_RDWR | O_CLOEXEC);
	unsigned int i;

	if (heap < 0)
		return -errno;

	for (i = 0; i < count; i++) {
		struct dma_heap_allocation_data data;

		memset(&data, 0, sizeof(data));
		data.len = size;
		data.fd_flags = O_RDWR | O_CLOEXEC;

		if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &data) < 0) {
			int e = errno;

			while (i--)
				close(fds[i]);
			close(heap);
			return -e;
		}
		fds[i] = (int)data.fd;
	}
	close(heap);
	return 0;
}
#endif