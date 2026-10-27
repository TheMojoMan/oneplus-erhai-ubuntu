// SPDX-License-Identifier: GPL-2.0-or-later

#ifndef IRIS_VAAPI_VK_COPY_H
#define IRIS_VAAPI_VK_COPY_H

#include <stddef.h>
#include <stdint.h>

struct iris_vk_copy;
struct iris_vk_job;

/* Create a Vulkan transfer engine capable of importing Linux DMA-BUFs.
 * Returns NULL when Vulkan or the required external-memory extensions are not
 * available. */
struct iris_vk_copy *iris_vk_copy_create(void);
void iris_vk_copy_destroy(struct iris_vk_copy *copy);

/* Copy @size bytes between DMA-BUFs and wait for the transfer to complete.
 * Imported buffer objects are cached by DMA-BUF identity, so the steady-state
 * path only records and submits a transfer command. */
int iris_vk_copy_dmabuf(struct iris_vk_copy *copy,
			uint64_t src_key, int src_fd, size_t src_size,
			uint64_t dst_key, int dst_fd, size_t dst_size,
			size_t size);

/* Asynchronous variant.  A submitted job owns its command buffer until the
 * caller observes completion and releases it. */
int iris_vk_copy_submit(struct iris_vk_copy *copy,
			uint64_t src_key, int src_fd, size_t src_size,
			uint64_t dst_key, int dst_fd, size_t dst_size,
			size_t size, struct iris_vk_job **job);
/* Return 1 when complete, 0 when not ready, or a negative errno. */
int iris_vk_copy_job_wait(struct iris_vk_copy *copy,
			  struct iris_vk_job *job, uint64_t timeout_ns);
void iris_vk_copy_job_release(struct iris_vk_job *job);
/* Drop a cached import after the caller has completed every job using it. */
void iris_vk_copy_forget(struct iris_vk_copy *copy, uint64_t key);

#endif
