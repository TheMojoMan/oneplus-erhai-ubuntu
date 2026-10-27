// SPDX-License-Identifier: GPL-2.0-or-later

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "vk_copy.h"

#ifdef IRIS_HAVE_VULKAN

#include <vulkan/vulkan.h>

#define IRIS_VK_MAX_BUFFERS 192
#define IRIS_VK_MAX_JOBS 32

struct iris_vk_buffer {
	VkBuffer buffer;
	VkDeviceMemory memory;
	uint64_t key;
	size_t size;
};

struct iris_vk_job {
	VkCommandBuffer command;
	VkFence fence;
	int in_use;
};

struct iris_vk_copy {
	pthread_mutex_t mutex;
	VkInstance instance;
	VkPhysicalDevice physical;
	VkDevice device;
	VkQueue queue;
	uint32_t queue_family;
	VkCommandPool command_pool;
	PFN_vkGetMemoryFdPropertiesKHR get_memory_fd_properties;
	struct iris_vk_buffer buffers[IRIS_VK_MAX_BUFFERS];
	unsigned int buffer_count;
	struct iris_vk_job jobs[IRIS_VK_MAX_JOBS];
};

static int
has_device_extension(VkPhysicalDevice physical, const char *name)
{
	VkExtensionProperties *props;
	uint32_t count = 0, i;
	int found = 0;

	if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, NULL) !=
	    VK_SUCCESS || !count)
		return 0;
	props = calloc(count, sizeof(*props));
	if (!props)
		return 0;
	if (vkEnumerateDeviceExtensionProperties(physical, NULL, &count, props) ==
	    VK_SUCCESS) {
		for (i = 0; i < count; i++) {
			if (!strcmp(props[i].extensionName, name)) {
				found = 1;
				break;
			}
		}
	}
	free(props);
	return found;
}

static int
select_physical_device(struct iris_vk_copy *copy)
{
	VkPhysicalDevice *devices;
	uint32_t count = 0, i;
	int ret = -ENODEV;

	if (vkEnumeratePhysicalDevices(copy->instance, &count, NULL) != VK_SUCCESS ||
	    !count)
		return -ENODEV;
	devices = calloc(count, sizeof(*devices));
	if (!devices)
		return -ENOMEM;
	if (vkEnumeratePhysicalDevices(copy->instance, &count, devices) !=
	    VK_SUCCESS) {
		free(devices);
		return -ENODEV;
	}

	for (i = 0; i < count; i++) {
		VkExternalBufferProperties external = {
			.sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES,
		};
		VkPhysicalDeviceExternalBufferInfo info = {
			.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
			.handleType =
				VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
			.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
				 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		};
		VkQueueFamilyProperties *families;
		uint32_t family_count = 0, j;

		if (!has_device_extension(devices[i],
				VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) ||
		    !has_device_extension(devices[i],
				VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) ||
		    !has_device_extension(devices[i],
				VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME))
			continue;
		vkGetPhysicalDeviceExternalBufferProperties(devices[i], &info,
						    &external);
		if (!(external.externalMemoryProperties.externalMemoryFeatures &
		      VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT))
			continue;

		vkGetPhysicalDeviceQueueFamilyProperties(devices[i],
						 &family_count, NULL);
		families = calloc(family_count, sizeof(*families));
		if (!families) {
			ret = -ENOMEM;
			break;
		}
		vkGetPhysicalDeviceQueueFamilyProperties(devices[i],
						 &family_count, families);
		for (j = 0; j < family_count; j++) {
			if (families[j].queueCount &&
			    (families[j].queueFlags &
			     (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT))) {
				copy->physical = devices[i];
				copy->queue_family = j;
				ret = 0;
				break;
			}
		}
		free(families);
		if (!ret)
			break;
	}
	free(devices);
	return ret;
}

struct iris_vk_copy *
iris_vk_copy_create(void)
{
	static const char *extensions[] = {
		VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
		VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
		VK_EXT_QUEUE_FAMILY_FOREIGN_EXTENSION_NAME,
	};
	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "iris-vaapi-copy",
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo instance_info = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkDeviceQueueCreateInfo queue_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueCount = 1,
	};
	VkDeviceCreateInfo device_info = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.enabledExtensionCount = sizeof(extensions) / sizeof(extensions[0]),
		.ppEnabledExtensionNames = extensions,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &queue_info,
	};
	VkCommandPoolCreateInfo pool_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	};
	VkCommandBufferAllocateInfo command_info = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = IRIS_VK_MAX_JOBS,
	};
	VkCommandBuffer commands[IRIS_VK_MAX_JOBS];
	VkFenceCreateInfo fence_info = {
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
	};
	VkPhysicalDeviceProperties props;
	struct iris_vk_copy *copy;
	float priority = 1.0f;
	int ret;

	copy = calloc(1, sizeof(*copy));
	if (!copy)
		return NULL;
	pthread_mutex_init(&copy->mutex, NULL);
	if (vkCreateInstance(&instance_info, NULL, &copy->instance) != VK_SUCCESS)
		goto fail;
	ret = select_physical_device(copy);
	if (ret)
		goto fail;

	queue_info.queueFamilyIndex = copy->queue_family;
	queue_info.pQueuePriorities = &priority;
	if (vkCreateDevice(copy->physical, &device_info, NULL, &copy->device) !=
	    VK_SUCCESS)
		goto fail;
	copy->get_memory_fd_properties =
		(PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(
			copy->device, "vkGetMemoryFdPropertiesKHR");
	if (!copy->get_memory_fd_properties)
		goto fail;
	vkGetDeviceQueue(copy->device, copy->queue_family, 0, &copy->queue);

	pool_info.queueFamilyIndex = copy->queue_family;
	if (vkCreateCommandPool(copy->device, &pool_info, NULL,
				&copy->command_pool) != VK_SUCCESS)
		goto fail;
	command_info.commandPool = copy->command_pool;
	if (vkAllocateCommandBuffers(copy->device, &command_info, commands) !=
	    VK_SUCCESS)
		goto fail;
	for (unsigned int i = 0; i < IRIS_VK_MAX_JOBS; i++) {
		copy->jobs[i].command = commands[i];
		if (vkCreateFence(copy->device, &fence_info, NULL,
				  &copy->jobs[i].fence) != VK_SUCCESS)
			goto fail;
	}

	vkGetPhysicalDeviceProperties(copy->physical, &props);
	fprintf(stderr, "iris-vaapi: Vulkan DMA-BUF copy enabled on %s\n",
		props.deviceName);
	return copy;

fail:
	fprintf(stderr,
		"iris-vaapi: Vulkan DMA-BUF copy unavailable; using CPU copy\n");
	iris_vk_copy_destroy(copy);
	return NULL;
}

static int
choose_memory_type(struct iris_vk_copy *copy, uint32_t bits)
{
	VkPhysicalDeviceMemoryProperties props;
	uint32_t i;

	vkGetPhysicalDeviceMemoryProperties(copy->physical, &props);
	for (i = 0; i < props.memoryTypeCount; i++) {
		if ((bits & (1U << i)) &&
		    (props.memoryTypes[i].propertyFlags &
		     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			return (int)i;
	}
	for (i = 0; i < props.memoryTypeCount; i++)
		if (bits & (1U << i))
			return (int)i;
	return -1;
}

static struct iris_vk_buffer *
import_buffer(struct iris_vk_copy *copy, uint64_t key, int fd, size_t size)
{
	VkExternalMemoryBufferCreateInfo external = {
		.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
		.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkBufferCreateInfo buffer_info = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.pNext = &external,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT |
			 VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkMemoryFdPropertiesKHR fd_props = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
	};
	VkMemoryRequirements requirements;
	VkMemoryDedicatedAllocateInfo dedicated = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
	};
	VkImportMemoryFdInfoKHR import = {
		.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
		.pNext = &dedicated,
		.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
	};
	VkMemoryAllocateInfo alloc = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.pNext = &import,
	};
	struct iris_vk_buffer *buffer;
	uint32_t memory_bits;
	int memory_type, import_fd;

	if (!key || !size)
		return NULL;
	for (unsigned int i = 0; i < copy->buffer_count; i++) {
		buffer = &copy->buffers[i];
		if (buffer->key == key && buffer->size == size)
			return buffer;
	}
	if (copy->buffer_count >= IRIS_VK_MAX_BUFFERS)
		return NULL;

	buffer = &copy->buffers[copy->buffer_count];
	if (vkCreateBuffer(copy->device, &buffer_info, NULL, &buffer->buffer) !=
	    VK_SUCCESS)
		return NULL;
	vkGetBufferMemoryRequirements(copy->device, buffer->buffer, &requirements);
	if (copy->get_memory_fd_properties(copy->device,
			VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
			&fd_props) != VK_SUCCESS)
		goto fail;
	memory_bits = requirements.memoryTypeBits & fd_props.memoryTypeBits;
	memory_type = choose_memory_type(copy, memory_bits);
	if (memory_type < 0 || requirements.size > size)
		goto fail;

	import_fd = dup(fd);
	if (import_fd < 0)
		goto fail;
	dedicated.buffer = buffer->buffer;
	import.fd = import_fd;
	alloc.allocationSize = requirements.size;
	alloc.memoryTypeIndex = (uint32_t)memory_type;
	if (vkAllocateMemory(copy->device, &alloc, NULL, &buffer->memory) !=
	    VK_SUCCESS) {
		/* Vulkan consumes the fd only when import succeeds. */
		close(import_fd);
		goto fail;
	}
	if (vkBindBufferMemory(copy->device, buffer->buffer, buffer->memory, 0) !=
	    VK_SUCCESS)
		goto fail;
	buffer->key = key;
	buffer->size = size;
	copy->buffer_count++;
	return buffer;

fail:
	if (buffer->memory)
		vkFreeMemory(copy->device, buffer->memory, NULL);
	if (buffer->buffer)
		vkDestroyBuffer(copy->device, buffer->buffer, NULL);
	memset(buffer, 0, sizeof(*buffer));
	return NULL;
}

static int
iris_vk_copy_submit_locked(struct iris_vk_copy *copy,
			uint64_t src_key, int src_fd, size_t src_size,
			uint64_t dst_key, int dst_fd, size_t dst_size,
			size_t size, struct iris_vk_job **job_out)
{
	VkCommandBufferBeginInfo begin = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	VkBufferMemoryBarrier acquire[2];
	VkBufferMemoryBarrier release[2];
	VkBufferCopy region = { .size = size };
	VkSubmitInfo submit = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
	};
	struct iris_vk_buffer *src, *dst;
	struct iris_vk_job *job = NULL;
	VkResult result;

	if (!copy || !job_out || !size || size > src_size || size > dst_size)
		return -EINVAL;
	for (unsigned int i = 0; i < IRIS_VK_MAX_JOBS; i++) {
		if (!__atomic_load_n(&copy->jobs[i].in_use,
				     __ATOMIC_ACQUIRE)) {
			job = &copy->jobs[i];
			break;
		}
	}
	if (!job)
		return -EAGAIN;
	src = import_buffer(copy, src_key, src_fd, src_size);
	dst = import_buffer(copy, dst_key, dst_fd, dst_size);
	if (!src || !dst)
		return -ENOTSUP;

	if (vkResetFences(copy->device, 1, &job->fence) != VK_SUCCESS)
		return -EIO;
	if (vkResetCommandBuffer(job->command, 0) != VK_SUCCESS)
		return -EIO;
	if (vkBeginCommandBuffer(job->command, &begin) != VK_SUCCESS)
		return -EIO;
	memset(acquire, 0, sizeof(acquire));
	memset(release, 0, sizeof(release));
	for (unsigned int i = 0; i < 2; i++) {
		acquire[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		acquire[i].srcQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		acquire[i].dstQueueFamilyIndex = copy->queue_family;
		acquire[i].offset = 0;
		acquire[i].size = VK_WHOLE_SIZE;
		release[i].sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
		release[i].srcQueueFamilyIndex = copy->queue_family;
		release[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_FOREIGN_EXT;
		release[i].offset = 0;
		release[i].size = VK_WHOLE_SIZE;
	}
	acquire[0].buffer = src->buffer;
	acquire[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acquire[1].buffer = dst->buffer;
	acquire[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	release[0].buffer = src->buffer;
	release[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	release[1].buffer = dst->buffer;
	release[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(job->command,
		VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
		0, 0, NULL, 2, acquire, 0, NULL);
	vkCmdCopyBuffer(job->command, src->buffer, dst->buffer, 1, &region);
	vkCmdPipelineBarrier(job->command,
		VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
		0, 0, NULL, 2, release, 0, NULL);
	if (vkEndCommandBuffer(job->command) != VK_SUCCESS)
		return -EIO;
	submit.pCommandBuffers = &job->command;
	result = vkQueueSubmit(copy->queue, 1, &submit, job->fence);
	if (result != VK_SUCCESS)
		return -EIO;
	__atomic_store_n(&job->in_use, 1, __ATOMIC_RELEASE);
	*job_out = job;
	return 0;
}

int
iris_vk_copy_submit(struct iris_vk_copy *copy,
			uint64_t src_key, int src_fd, size_t src_size,
			uint64_t dst_key, int dst_fd, size_t dst_size,
			size_t size, struct iris_vk_job **job_out)
{
	int ret;

	if (!copy)
		return -EINVAL;
	pthread_mutex_lock(&copy->mutex);
	ret = iris_vk_copy_submit_locked(copy, src_key, src_fd, src_size,
					 dst_key, dst_fd, dst_size, size,
					 job_out);
	pthread_mutex_unlock(&copy->mutex);
	return ret;
}

int
iris_vk_copy_job_wait(struct iris_vk_copy *copy, struct iris_vk_job *job,
			  uint64_t timeout_ns)
{
	VkResult result;

	if (!copy || !job || !__atomic_load_n(&job->in_use,
					      __ATOMIC_ACQUIRE))
		return -EINVAL;
	if (!timeout_ns)
		result = vkGetFenceStatus(copy->device, job->fence);
	else
		result = vkWaitForFences(copy->device, 1, &job->fence, VK_TRUE,
					 timeout_ns);
	if (result == VK_SUCCESS)
		return 1;
	if (result == VK_NOT_READY || result == VK_TIMEOUT)
		return 0;
	return -EIO;
}

void
iris_vk_copy_job_release(struct iris_vk_job *job)
{
	if (job)
		__atomic_store_n(&job->in_use, 0, __ATOMIC_RELEASE);
}

static void
iris_vk_copy_forget_locked(struct iris_vk_copy *copy, uint64_t key)
{
	unsigned int i;

	if (!copy || !key)
		return;
	for (i = 0; i < copy->buffer_count; i++) {
		if (copy->buffers[i].key != key)
			continue;
		vkDestroyBuffer(copy->device, copy->buffers[i].buffer, NULL);
		vkFreeMemory(copy->device, copy->buffers[i].memory, NULL);
		copy->buffers[i] = copy->buffers[copy->buffer_count - 1];
		memset(&copy->buffers[copy->buffer_count - 1], 0,
		       sizeof(copy->buffers[0]));
		copy->buffer_count--;
		return;
	}
}

void
iris_vk_copy_forget(struct iris_vk_copy *copy, uint64_t key)
{
	if (!copy || !key)
		return;
	pthread_mutex_lock(&copy->mutex);
	iris_vk_copy_forget_locked(copy, key);
	pthread_mutex_unlock(&copy->mutex);
}

int
iris_vk_copy_dmabuf(struct iris_vk_copy *copy,
			uint64_t src_key, int src_fd, size_t src_size,
			uint64_t dst_key, int dst_fd, size_t dst_size,
			size_t size)
{
	struct iris_vk_job *job;
	int ret;

	ret = iris_vk_copy_submit(copy, src_key, src_fd, src_size,
				  dst_key, dst_fd, dst_size, size, &job);
	if (ret)
		return ret;
	ret = iris_vk_copy_job_wait(copy, job, UINT64_MAX);
	iris_vk_copy_job_release(job);
	return ret == 1 ? 0 : ret;
}

void
iris_vk_copy_destroy(struct iris_vk_copy *copy)
{
	unsigned int i;

	if (!copy)
		return;
	if (copy->device)
		vkDeviceWaitIdle(copy->device);
	for (i = 0; i < IRIS_VK_MAX_JOBS; i++)
		if (copy->jobs[i].fence)
			vkDestroyFence(copy->device, copy->jobs[i].fence, NULL);
	for (i = 0; i < copy->buffer_count; i++) {
		if (copy->buffers[i].buffer)
			vkDestroyBuffer(copy->device, copy->buffers[i].buffer, NULL);
		if (copy->buffers[i].memory)
			vkFreeMemory(copy->device, copy->buffers[i].memory, NULL);
	}
	if (copy->command_pool)
		vkDestroyCommandPool(copy->device, copy->command_pool, NULL);
	if (copy->device)
		vkDestroyDevice(copy->device, NULL);
	if (copy->instance)
		vkDestroyInstance(copy->instance, NULL);
	pthread_mutex_destroy(&copy->mutex);
	free(copy);
}

#else

struct iris_vk_copy *
iris_vk_copy_create(void)
{
	fprintf(stderr,
		"iris-vaapi: built without Vulkan support; using CPU copy\n");
	return NULL;
}

void
iris_vk_copy_destroy(struct iris_vk_copy *copy)
{
	(void)copy;
}

int
iris_vk_copy_submit(struct iris_vk_copy *copy,
			uint64_t src_key, int src_fd, size_t src_size,
			uint64_t dst_key, int dst_fd, size_t dst_size,
			size_t size, struct iris_vk_job **job)
{
	(void)copy;
	(void)src_key;
	(void)src_fd;
	(void)src_size;
	(void)dst_key;
	(void)dst_fd;
	(void)dst_size;
	(void)size;
	(void)job;
	return -ENOTSUP;
}

int
iris_vk_copy_job_wait(struct iris_vk_copy *copy, struct iris_vk_job *job,
			  uint64_t timeout_ns)
{
	(void)copy;
	(void)job;
	(void)timeout_ns;
	return -ENOTSUP;
}

void
iris_vk_copy_job_release(struct iris_vk_job *job)
{
	(void)job;
}

void
iris_vk_copy_forget(struct iris_vk_copy *copy, uint64_t key)
{
	(void)copy;
	(void)key;
}

int
iris_vk_copy_dmabuf(struct iris_vk_copy *copy,
			uint64_t src_key, int src_fd, size_t src_size,
			uint64_t dst_key, int dst_fd, size_t dst_size,
			size_t size)
{
	(void)copy;
	(void)src_key;
	(void)src_fd;
	(void)src_size;
	(void)dst_key;
	(void)dst_fd;
	(void)dst_size;
	(void)size;
	return -ENOTSUP;
}

#endif
