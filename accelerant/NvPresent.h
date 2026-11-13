#pragma once

/*
 * The driver half of GPU presentation (headers/GpuPresent.h, proposal R-1 in
 * docs/RENGA-REQUESTS.md) on top of NVKMS.
 *
 * Buffers come from a client (a Vulkan WSI) as RM memory exported to a file
 * descriptor, the form NVK's vkGetMemoryFdKHR produces. They are registered
 * with NVKMS as surfaces and flipped onto a head's layers. Flip completion is
 * reported through NVKMS completion notifiers with "awaken", which makes
 * NVKMS queue NVKMS_EVENT_TYPE_FLIP_OCCURRED on this client's event queue;
 * the queue is readable on the modeset device's descriptor, which Quasar's
 * kernel side already supports select() on (KmsDevice.cpp).
 *
 * Vblank events use NVKMS_IOCTL_NOTIFY_VBLANK, which is one-shot and signals
 * a separate "unicast event" open of the modeset device; that descriptor
 * stays readable after it fires. Each request therefore gets its own
 * descriptor, closed once seen.
 *
 * A helper thread waits on the modeset descriptor and the pending vblank
 * descriptors, turns what it sees into gpu_present_events, and signals one
 * eventfd (Renga's libgnu). event_fd() returns that eventfd, so app_server
 * waits on a single descriptor for both kinds of event, in epoll with its
 * other sources.
 */

#include <map>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>
#include <atomic>

#include <NvRmApi.h>
#include <NvRmDevice.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>
#include <NvKmsSurface.h>

// Renga installs the interface as <add-ons/graphics/GpuPresent.h> (F-58,
// 744fa9b4). headers/GpuPresent.h is a verbatim copy of it, for building
// against a Renga tree from before that commit; both use the same include
// guard, so only one is ever seen.
#if __has_include(<add-ons/graphics/GpuPresent.h>)
#include <add-ons/graphics/GpuPresent.h>
#else
#include <GpuPresent.h>
#endif


class NvPresent {
public:
	// What the presenter needs from the accelerant. scanoutMemory returns
	// the RM handle and size of the memory shown on a head's primary layer,
	// or 0 when none is.
	struct Context {
		NvRmDevice *rmDev;
		NvKmsApi *kms;
		NvKmsDevice *kmsDev;
		NvKmsDispHandle disp;
		NvU32 headCount;
		NvHandle (*scanoutMemory)(NvU32 head, gpu_buffer_desc *desc);
	};

	explicit NvPresent(const Context &context);
	~NvPresent();

	NvPresent(const NvPresent &) = delete;
	NvPresent &operator=(const NvPresent &) = delete;

	static const gpu_present_hooks *Hooks();
	static void SetInstance(NvPresent *present);

	status_t GetCaps(uint32 head, gpu_present_caps *caps);
	status_t ImportBuffer(const gpu_buffer_desc *desc, uint32 *_id);
	status_t ReleaseBuffer(uint32 id);
	status_t Flip(uint32 head, uint32 plane, uint32 id,
		const gpu_flip_params *params);
	status_t DisablePlane(uint32 head, uint32 plane);
	status_t RequestVblank(uint32 head, bool repeat);
	int32 EventFd();
	status_t NextEvent(gpu_present_event *event);
	status_t ExportScanout(uint32 head, gpu_buffer_desc *desc);

private:
	struct Buffer {
		NvKmsSurface surface;
		gpu_buffer_desc desc;
		uint32 useCount = 0;	// flips that may still show it
		bool released = false;
	};

	struct PlaneState {
		uint32 shown = 0;		// buffer id on screen, 0 = none
		uint32 pending = 0;		// buffer id of a flip not yet complete
		uint64 pendingUserData = 0;
		uint32 nextNotifier = 0;
		// A flip waiting for its wait_fd: pending is set, the flip is not
		// yet submitted to NVKMS. waitFd is our duplicate of the caller's
		// descriptor, closed by the helper thread or DisablePlane().
		int waitFd = -1;
		gpu_flip_params waitParams {};
	};

	void SetUpNotifiers();
	void EventThread();
	void DrainKmsEvents();	// fLock held
	void Signal();
	void Wake();
	uint32 NotifierOffsetInWords(uint32 head, uint32 plane, uint32 index) const;
	bool NotifierDone(uint32 head, uint32 plane, uint32 index) const;
	void ResetNotifier(uint32 head, uint32 plane, uint32 index);
	void DropBufferUse(uint32 id);
	PlaneState *Plane(uint32 head, uint32 plane);
	status_t SubmitFlip(uint32 head, uint32 plane, NvKmsSurfaceHandle surface,
		const gpu_flip_params *params, bool withNotifier, uint32 notifier);
	status_t StartFlip(uint32 head, uint32 plane, PlaneState &state,
		const gpu_flip_params &params);	// fLock held
	void CompleteFlipNow(uint32 head, uint32 plane, PlaneState &state,
		uint64 userData);	// fLock held
	status_t ArmVblank(uint32 head, bool repeat);	// fLock held

	Context fContext;
	std::mutex fLock;

	std::map<uint32, Buffer> fBuffers;
	uint32 fNextId = 1;
	std::map<std::pair<uint32, uint32>, PlaneState> fPlanes;
	std::deque<gpu_present_event> fEvents;

	// Completion notifiers: NVKMS writes flip completion into a small NISO
	// surface; slot layout as in NVIDIA's nvkms-kapi-notifiers.
	NvRmObject fNotifierMemory;
	NvRmMemoryMapping fNotifierMapping;
	NvKmsSurface fNotifierSurface;
	enum NvKmsNIsoFormat fNotifierFormat {};
	bool fHaveNotifiers = false;
	bool fEventsDeclared = false;

	// Event delivery.
	int fSignalFd = -1;		// eventfd returned by event_fd()
	int fWakeFd = -1;		// eventfd that wakes the helper thread
	struct PendingVblank {
		int fd;
		uint32 head;
		bool repeating;	// NVKMS keeps notifying fd at every vblank
	};
	std::vector<PendingVblank> fPendingVblanks;	// fLock
	std::vector<bool> fRepeatVblank;	// per head, fLock
	std::thread fThread;
	std::atomic<bool> fQuit {false};
};
