/*
 * Regression test for the event-sync NVK hangs fixed by mesa-nvk patch 0008,
 * at the Vulkan level, no window needed.
 *
 * Background: the nvrm sync type has no wait-before-signal support, so once a
 * queue has had a submit waiting on a timeline point not yet submitted, Mesa's
 * runtime runs that queue with a submit thread. From then on it moves every
 * binary-semaphore wait into a temporary vk_sync, and destroys the temporary
 * right after the driver accepts the submission.
 *
 * Part 1, stale reset: in immediate mode the runtime resets a binary
 * semaphore right after submitting its wait, usually before the GPU has run
 * the signal. NVK used to leave the semaphore looking signaled, so after the
 * switch a wait whose signal was still queued in the submit thread was moved
 * too early, and its acquire waited on a slot the signal never wrote.
 *
 * Part 2, slot reuse: a binary semaphore is signaled and the signal completes,
 * then slow work and a submit waiting on it are queued. The moved wait's
 * temporary is destroyed at once; NVK used to hand its slot to the next
 * allocation and zero it while the GPU acquire on it was still queued behind
 * the slow work.
 *
 * Without the fix either part hangs the queue; the test then fails with a
 * timeout. Exit status 0 = pass, 1 = hang or error, 2 = setup failure.
 * --no-thread runs part 2 with the queue left in immediate mode (control).
 *
 * Build: cc -O2 -Wall -o vk-binary-wait-reuse vk-binary-wait-reuse.c -lvulkan -lpthread
 * (on Haiku, pthreads are in libroot: drop -lpthread)
 */
#include <inttypes.h>
#include <pthread.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define CHECK_VK(expr) do { \
	VkResult r_ = (expr); \
	if (r_ != VK_SUCCESS) { \
		fprintf(stderr, "setup failed: %s = %d (line %d)\n", #expr, r_, __LINE__); \
		exit(2); \
	} \
} while (0)

static const uint32_t kRounds = 50;
static const VkDeviceSize kBufferSize = 256ull << 20; /* 256 MiB */
static const uint32_t kFillsPerSlowSubmit = 24;       /* ~6 GiB of writes, some ms */
static const uint64_t kTimeoutNs = UINT64_C(10) * 1000000000u;

static VkDevice gDev;
static VkQueue gQueue;

/* One vkQueueSubmit with at most one wait and one signal. A value of 0 means
 * a binary semaphore, anything else a timeline point. */
static void
submit(VkSemaphore wait, uint64_t waitValue, VkCommandBuffer cb,
       VkSemaphore signal, uint64_t signalValue)
{
	VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkTimelineSemaphoreSubmitInfo tsi = {
		.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO,
		.waitSemaphoreValueCount = wait != VK_NULL_HANDLE,
		.pWaitSemaphoreValues = &waitValue,
		.signalSemaphoreValueCount = signal != VK_NULL_HANDLE,
		.pSignalSemaphoreValues = &signalValue,
	};
	VkSubmitInfo si = {
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.pNext = &tsi,
		.waitSemaphoreCount = wait != VK_NULL_HANDLE,
		.pWaitSemaphores = &wait,
		.pWaitDstStageMask = &stage,
		.commandBufferCount = cb != VK_NULL_HANDLE,
		.pCommandBuffers = &cb,
		.signalSemaphoreCount = signal != VK_NULL_HANDLE,
		.pSignalSemaphores = &signal,
	};
	CHECK_VK(vkQueueSubmit(gQueue, 1, &si, VK_NULL_HANDLE));
}

/* Wait for a timeline point; on timeout report the hang and exit. */
static void
expect_reached(VkSemaphore timeline, uint64_t value, const char *what)
{
	VkSemaphoreWaitInfo swi = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
		.semaphoreCount = 1,
		.pSemaphores = &timeline,
		.pValues = &value,
	};
	VkResult r = vkWaitSemaphores(gDev, &swi, kTimeoutNs);
	if (r == VK_SUCCESS)
		return;
	uint64_t reached = 0;
	vkGetSemaphoreCounterValue(gDev, timeline, &reached);
	printf("FAIL: %s: queue stalled (timeline at %" PRIu64 ", wanted %" PRIu64
	       ", vkWaitSemaphores %d after %" PRIu64 " s)\n", what, reached, value,
	       r, kTimeoutNs / UINT64_C(1000000000));
	fflush(stdout);
	_exit(1);   /* don't tear down a stuck device */
}

static VkSemaphore
create_semaphore(bool timeline)
{
	VkSemaphoreTypeCreateInfo type = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO,
		.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE,
	};
	VkSemaphoreCreateInfo sci = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
		.pNext = timeline ? &type : NULL,
	};
	VkSemaphore sem;
	CHECK_VK(vkCreateSemaphore(gDev, &sci, NULL, &sem));
	return sem;
}

/* Signals a timeline point from another thread after a delay. */
struct delayed_signal { VkSemaphore sem; uint64_t value; unsigned delay_ms; };

static void *
delayed_signal_thread(void *arg)
{
	struct delayed_signal *ds = arg;
	usleep(ds->delay_ms * 1000);
	VkSemaphoreSignalInfo ssi = {
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO,
		.semaphore = ds->sem,
		.value = ds->value,
	};
	CHECK_VK(vkSignalSemaphore(gDev, &ssi));
	return NULL;
}

int main(int argc, char **argv)
{
	bool forceThread = true;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--no-thread") == 0)
			forceThread = false;
	}

	VkApplicationInfo app = {
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "vk-binary-wait-reuse",
		.apiVersion = VK_API_VERSION_1_2,
	};
	VkInstanceCreateInfo ici = {
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app,
	};
	VkInstance instance;
	CHECK_VK(vkCreateInstance(&ici, NULL, &instance));

	uint32_t pdCount = 0;
	CHECK_VK(vkEnumeratePhysicalDevices(instance, &pdCount, NULL));
	if (pdCount == 0) {
		fprintf(stderr, "setup failed: no Vulkan device\n");
		return 2;
	}
	VkPhysicalDevice *pds = calloc(pdCount, sizeof(*pds));
	CHECK_VK(vkEnumeratePhysicalDevices(instance, &pdCount, pds));
	VkPhysicalDevice pd = pds[0];
	for (uint32_t i = 0; i < pdCount; i++) {
		VkPhysicalDeviceProperties p;
		vkGetPhysicalDeviceProperties(pds[i], &p);
		if (strstr(p.deviceName, "NVK") != NULL) {
			pd = pds[i];
			break;
		}
	}
	VkPhysicalDeviceProperties props;
	vkGetPhysicalDeviceProperties(pd, &props);
	printf("device: %s\n", props.deviceName);

	uint32_t qfCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, NULL);
	VkQueueFamilyProperties *qfs = calloc(qfCount, sizeof(*qfs));
	vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs);
	uint32_t qf = UINT32_MAX;
	for (uint32_t i = 0; i < qfCount; i++) {
		if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
			qf = i;
			break;
		}
	}
	if (qf == UINT32_MAX) {
		fprintf(stderr, "setup failed: no graphics queue\n");
		return 2;
	}

	float prio = 1.0f;
	VkDeviceQueueCreateInfo qci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = qf,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	VkPhysicalDeviceVulkan12Features f12 = {
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
		.timelineSemaphore = VK_TRUE,
	};
	VkDeviceCreateInfo dci = {
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.pNext = &f12,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
	};
	CHECK_VK(vkCreateDevice(pd, &dci, NULL, &gDev));
	VkDevice dev = gDev;
	vkGetDeviceQueue(dev, qf, 0, &gQueue);

	/* A large device-local buffer for slow fills. */
	VkBufferCreateInfo bci = {
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = kBufferSize,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	VkBuffer buf;
	CHECK_VK(vkCreateBuffer(dev, &bci, NULL, &buf));
	VkMemoryRequirements req;
	vkGetBufferMemoryRequirements(dev, buf, &req);
	VkPhysicalDeviceMemoryProperties mp;
	vkGetPhysicalDeviceMemoryProperties(pd, &mp);
	uint32_t memType = UINT32_MAX;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if ((req.memoryTypeBits & (1u << i)) &&
		    (mp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
			memType = i;
			break;
		}
	}
	if (memType == UINT32_MAX) {
		fprintf(stderr, "setup failed: no device-local memory type\n");
		return 2;
	}
	VkMemoryAllocateInfo mai = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = req.size,
		.memoryTypeIndex = memType,
	};
	VkDeviceMemory mem;
	CHECK_VK(vkAllocateMemory(dev, &mai, NULL, &mem));
	CHECK_VK(vkBindBufferMemory(dev, buf, mem, 0));

	VkCommandPoolCreateInfo cpci = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.queueFamilyIndex = qf,
	};
	VkCommandPool pool;
	CHECK_VK(vkCreateCommandPool(dev, &cpci, NULL, &pool));
	VkCommandBufferAllocateInfo cbai = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 2,
	};
	VkCommandBuffer cbs[2];
	CHECK_VK(vkAllocateCommandBuffers(dev, &cbai, cbs));
	VkCommandBuffer slowCb = cbs[0], quickCb = cbs[1];

	/* Recorded once, submitted many times (simultaneous use is not needed:
	 * each submit of the same buffer is ordered behind the previous one on
	 * the single queue, but mark it anyway so validation stays quiet). */
	VkCommandBufferBeginInfo cbbi = {
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
	};
	VkMemoryBarrier barrier = {
		.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
	};
	CHECK_VK(vkBeginCommandBuffer(slowCb, &cbbi));
	for (uint32_t i = 0; i < kFillsPerSlowSubmit; i++) {
		vkCmdFillBuffer(slowCb, buf, 0, VK_WHOLE_SIZE, 0x01010101u * (i + 1));
		vkCmdPipelineBarrier(slowCb, VK_PIPELINE_STAGE_TRANSFER_BIT,
		                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &barrier,
		                     0, NULL, 0, NULL);
	}
	CHECK_VK(vkEndCommandBuffer(slowCb));
	CHECK_VK(vkBeginCommandBuffer(quickCb, &cbbi));
	vkCmdFillBuffer(quickCb, buf, 0, 4096, 0xdeadbeefu);
	CHECK_VK(vkEndCommandBuffer(quickCb));

	VkSemaphore binary = create_semaphore(false);
	VkSemaphore done = create_semaphore(true);
	uint64_t doneValue = 0;

	if (forceThread) {
		/* Part 1a, immediate mode: signal/wait rounds on the binary semaphore
		 * without waiting, so the runtime resets it while its signal is still
		 * queued on the GPU. */
		for (uint32_t i = 0; i < 4; i++) {
			submit(VK_NULL_HANDLE, 0, slowCb, binary, 0);
			submit(binary, 0, quickCb, done, ++doneValue);
		}
		expect_reached(done, doneValue, "part 1, immediate-mode rounds");

		/* Switch the queue to submit-thread mode: a wait-before-signal. */
		VkSemaphore gate = create_semaphore(true);
		struct delayed_signal openGate = { gate, 1, 100 };
		pthread_t t;
		pthread_create(&t, NULL, delayed_signal_thread, &openGate);
		submit(gate, 1, VK_NULL_HANDLE, VK_NULL_HANDLE, 0);
		pthread_join(t, NULL);
		CHECK_VK(vkQueueWaitIdle(gQueue));
		printf("queue switched to submit-thread mode (wait-before-signal)\n");

		/* Part 1b: a signal held back in the submit thread (it waits on hold),
		 * then a wait on the binary semaphore. The wait must not be moved
		 * until that signal has been emitted. */
		VkSemaphore hold = create_semaphore(true);
		struct delayed_signal release = { hold, 1, 300 };
		pthread_create(&t, NULL, delayed_signal_thread, &release);
		submit(hold, 1, quickCb, binary, 0);
		submit(binary, 0, quickCb, done, ++doneValue);
		pthread_join(t, NULL);
		expect_reached(done, doneValue, "part 1, wait behind a held-back signal");
		printf("part 1 (stale reset) passed\n");
		vkDestroySemaphore(dev, hold, NULL);
		vkDestroySemaphore(dev, gate, NULL);
	} else {
		printf("control run: queue left in immediate mode\n");
	}

	/* Part 2: completed signal, then slow work and a wait on it; while the
	 * GPU is still busy, allocate semaphores so a freed slot would be reused
	 * (and zeroed) at once. */
	for (uint32_t r = 0; r < kRounds; r++) {
		submit(VK_NULL_HANDLE, 0, quickCb, binary, 0);
		submit(VK_NULL_HANDLE, 0, VK_NULL_HANDLE, done, ++doneValue);
		expect_reached(done, doneValue, "part 2, signal");

		submit(VK_NULL_HANDLE, 0, slowCb, VK_NULL_HANDLE, 0);
		submit(binary, 0, quickCb, done, ++doneValue);

		/* Let the submit thread hand both to the driver and destroy the
		 * temporary, then allocate while the slow work runs. */
		VkSemaphore scratch[16];
		for (uint32_t k = 0; k < 16; k++) {
			usleep(200);
			scratch[k] = create_semaphore(false);
		}
		for (uint32_t k = 0; k < 16; k++)
			vkDestroySemaphore(dev, scratch[k], NULL);

		expect_reached(done, doneValue, "part 2, wait queued behind slow work");
	}
	printf("PASS: %s%" PRIu32 " slot-reuse rounds completed\n",
	       forceThread ? "stale-reset check and " : "", kRounds);

	vkDeviceWaitIdle(dev);
	vkDestroySemaphore(dev, binary, NULL);
	vkDestroySemaphore(dev, done, NULL);
	vkDestroyCommandPool(dev, pool, NULL);
	vkDestroyBuffer(dev, buf, NULL);
	vkFreeMemory(dev, mem, NULL);
	vkDestroyDevice(dev, NULL);
	vkDestroyInstance(instance, NULL);
	free(qfs);
	free(pds);
	return 0;
}
