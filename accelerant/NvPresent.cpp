/*
 * GPU presentation on NVKMS; see NvPresent.h and headers/GpuPresent.h.
 *
 * Status: written against nvkms-api.h and NVIDIA's own NVKMS client
 * (src/nvidia-modeset/kapi, the code nvidia-drm uses on Linux), syntax-
 * checked for Haiku, not run. The parts that only the hardware can confirm
 * are marked "UNVERIFIED".
 */

#include "NvPresent.h"

#include <stdint.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>
// Renga's eventfd() is in libgnu, with its header in develop/headers/gnu.
// If the include path misses that directory, declare what is used here
// rather than fail: the declarations are Renga's (headers/compatibility/
// gnu/sys/eventfd.h) and the functions come from libgnu at link time.
#if __has_include(<sys/eventfd.h>)
#include <sys/eventfd.h>
#else
#include <stdint.h>
typedef uint64_t eventfd_t;
#define EFD_CLOEXEC		O_CLOEXEC
#define EFD_NONBLOCK	O_NONBLOCK
extern "C" {
int eventfd(unsigned int initialValue, int flags);
int eventfd_read(int fd, eventfd_t *value);
int eventfd_write(int fd, eventfd_t value);
}
#endif

#include <ErrorUtils.h>

#include "NvUtils.h"

extern "C" {
#include "nvkms-sync.h"
}
#include "nvkms-haiku.h"	// NVIDIA_MODESET_DEVICE_NAME


// Slots per layer, slot size and layout, as in NVIDIA's
// kapi/include/nvkms-kapi-notifiers.h: two notifiers per (head, layer), so
// the next flip never reuses the notifier of the flip still on screen.
static const uint32 kNotifiersPerLayer = 2;
static const uint32 kNotifierSize = 0x10;
static const uint32 kNotifierSurfaceSize = 0x1000;

static_assert(NVKMS_MAX_HEADS_PER_DISP * NVKMS_MAX_LAYERS_PER_HEAD
	* kNotifiersPerLayer * kNotifierSize <= kNotifierSurfaceSize,
	"notifier surface too small");

// DRM format modifiers for NVIDIA block-linear memory (drm_fourcc.h):
// vendor 0x03 in bits 56-63, 0x10 marks the 2D block-linear form, and the
// low four bits are log2 of the block height in GOBs.
static const uint64 kModVendorShift = 56;
static const uint64 kModVendorNvidia = 0x03;


static NvPresent *sPresent = nullptr;


static inline status_t ToStatus(const std::system_error &ex)
{
	if (ex.code().category() == std::generic_category())
		return ex.code().value();
	return B_ERROR;
}


NvPresent::NvPresent(const Context &context):
	fContext(context),
	fRepeatVblank(context.headCount, false)
{
	try {
		SetUpNotifiers();
	} catch (const std::system_error &ex) {
		// Flips still work without notifiers; only completion events are
		// lost, and Flip() refuses to queue a second flip while it cannot
		// tell the first has completed. Reported, not fatal.
		debug_printf("[!] NvPresent: no flip notifiers (%s)\n", ex.what());
		fHaveNotifiers = false;
	}

	NvKmsDeclareEventInterestParams params {};
	params.request.interestMask = 1U << NVKMS_EVENT_TYPE_FLIP_OCCURRED;
	fEventsDeclared = fContext.kms->Control(NVKMS_IOCTL_DECLARE_EVENT_INTEREST,
		&params, sizeof(params)) >= 0;

	fSignalFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	fWakeFd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
	if (fSignalFd < 0 || fWakeFd < 0) {
		debug_printf("[!] NvPresent: eventfd failed (%s); no event delivery\n",
			strerror(errno));
		return;
	}
	fThread = std::thread([this] { EventThread(); });
}


NvPresent::~NvPresent()
{
	if (sPresent == this)
		sPresent = nullptr;

	if (fThread.joinable()) {
		fQuit = true;
		Wake();
		fThread.join();
	}
	for (auto &it : fPlanes) {
		if (it.second.waitFd >= 0)
			close(it.second.waitFd);
	}
	for (const PendingVblank &pending : fPendingVblanks)
		close(pending.fd);
	if (fSignalFd >= 0)
		close(fSignalFd);
	if (fWakeFd >= 0)
		close(fWakeFd);

	// Surfaces unregister themselves (NvKmsSurface). Planes showing an
	// imported buffer are left as they are: the accelerant is going away,
	// and app_server sets a mode with its own framebuffer before it reuses
	// the display.
}


void
NvPresent::SetUpNotifiers()
{
	const NvKmsAllocDeviceReply &info = fContext.kmsDev->Info();

	// NVDisplay (Volta and newer) uses the four-word NVDisplay format; take
	// the newest format the device supports.
	static const NvKmsNIsoFormat kPreferred[] = {
		NVKMS_NISO_FORMAT_FOUR_WORD_NVDISPLAY,
		NVKMS_NISO_FORMAT_FOUR_WORD,
		NVKMS_NISO_FORMAT_LEGACY,
	};
	bool found = false;
	for (NvKmsNIsoFormat format : kPreferred) {
		if ((info.validNIsoFormatMask & (1 << format)) != 0) {
			fNotifierFormat = format;
			found = true;
			break;
		}
	}
	if (!found)
		RaiseErrno(ENOTSUP);

	NvU8 compressible = 0;
	nvKmsKapiAllocateVideoMemory(*fContext.rmDev, fNotifierMemory,
		NvKmsSurfaceMemoryLayoutPitch, kNotifierSurfaceSize,
		NVKMS_KAPI_ALLOCATION_TYPE_NOTIFIER, &compressible);

	fNotifierMapping = fContext.rmDev->MapMemory(fNotifierMemory.Get(), false,
		0, kNotifierSurfaceSize, 0);

	FileDesc memoryFd = fContext.rmDev->ExportObjectToFd(
		fContext.rmDev->Device().Get(), fNotifierMemory.Get());

	// Registered as NVIDIA's own client registers its notifier surface
	// (kapi/src/nvkms-kapi-notifiers.c, AllocateNisoSurface): one row of
	// I8 pixels, NISO.
	NvKmsRegisterSurfaceParams params {};
	params.request.deviceHandle = fContext.kmsDev->Get();
	params.request.useFd = true;
	params.request.planes[0].u.fd = memoryFd.Get();
	params.request.planes[0].pitch = kNotifierSurfaceSize;
	params.request.planes[0].rmObjectSizeInBytes = kNotifierSurfaceSize;
	params.request.widthInPixels = kNotifierSurfaceSize;
	params.request.heightInPixels = 1;
	params.request.layout = NvKmsSurfaceMemoryLayoutPitch;
	params.request.format = NvKmsSurfaceMemoryFormatI8;
	params.request.isoType = NVKMS_MEMORY_NISO;
	CheckErrno(fContext.kms->Control(NVKMS_IOCTL_REGISTER_SURFACE, &params,
		sizeof(params)));
	fNotifierSurface = NvKmsSurface(*fContext.kmsDev, params.reply.surfaceHandle);

	memset(fNotifierMapping.Address(), 0, kNotifierSurfaceSize);
	for (uint32 head = 0; head < fContext.headCount; head++) {
		for (uint32 layer = 0; layer < NVKMS_MAX_LAYERS_PER_HEAD; layer++) {
			for (uint32 i = 0; i < kNotifiersPerLayer; i++)
				ResetNotifier(head, layer, i);
		}
	}
	fHaveNotifiers = true;
}


static inline uint32
NotifierIndex(uint32 head, uint32 plane, uint32 index)
{
	return (head * NVKMS_MAX_LAYERS_PER_HEAD + plane) * kNotifiersPerLayer
		+ index;
}


uint32
NvPresent::NotifierOffsetInWords(uint32 head, uint32 plane, uint32 index) const
{
	return NotifierIndex(head, plane, index) * kNotifierSize / 4;
}


void
NvPresent::ResetNotifier(uint32 head, uint32 plane, uint32 index)
{
	nvKmsResetNotifier(fNotifierFormat, plane != NVKMS_MAIN_LAYER,
		NotifierIndex(head, plane, index), fNotifierMapping.Address());
}


bool
NvPresent::NotifierDone(uint32 head, uint32 plane, uint32 index) const
{
	nvKmsParsedNotifier parsed {};
	nvKmsParseNotifier(fNotifierFormat, plane != NVKMS_MAIN_LAYER,
		NotifierIndex(head, plane, index), fNotifierMapping.Address(), &parsed);
	return parsed.status == NVKMS_NOTIFIER_STATUS_FINISHED;
}


NvPresent::PlaneState *
NvPresent::Plane(uint32 head, uint32 plane)
{
	if (head >= fContext.headCount
		|| plane >= fContext.kmsDev->Info().numLayers[head])
		return nullptr;
	return &fPlanes[{head, plane}];
}


void
NvPresent::DropBufferUse(uint32 id)
{
	auto it = fBuffers.find(id);
	if (it == fBuffers.end())
		return;
	if (it->second.useCount > 0)
		it->second.useCount--;
	if (it->second.useCount == 0 && it->second.released)
		fBuffers.erase(it);
}


status_t
NvPresent::GetCaps(uint32 head, gpu_present_caps *caps)
{
	if (head >= fContext.headCount || caps == nullptr)
		return B_BAD_VALUE;

	const NvKmsAllocDeviceReply &info = fContext.kmsDev->Info();
	*caps = {};
	caps->planes_per_head = info.numLayers[head];
	caps->max_width = info.maxWidthInPixels;
	caps->max_height = info.maxHeightInPixels;
	// Scaling (and placing) layers other than the main one needs NVKMS
	// "window mode" (NVDisplay). Renga's header has no separate flag for
	// placing an overlay plane: a plane that takes a destination rectangle
	// is assumed to be placeable. VRR depends on the monitor and the mode;
	// the flag says the driver can ask for it.
	// UNVERIFIED on the RTX 5070: the layer count and window mode support.
	caps->flags = B_GPU_PRESENT_VRR | B_GPU_PRESENT_ASYNC_FLIP;
	for (uint32 layer = 1; layer < caps->planes_per_head; layer++) {
		if (info.layerCaps[layer].supportsWindowMode) {
			caps->flags |= B_GPU_PRESENT_SCALING;
			break;
		}
	}
	return B_OK;
}


status_t
NvPresent::ImportBuffer(const gpu_buffer_desc *desc, uint32 *_id)
{
	if (desc == nullptr || _id == nullptr
		|| desc->handle_type != B_GPU_BUFFER_FD || desc->handle < 0
		|| desc->width == 0 || desc->height == 0
		|| desc->width > 0xffff || desc->height > 0xffff)
		return B_BAD_VALUE;

	NvKmsSurfaceMemoryFormat format;
	switch (desc->format) {
		case B_RGB32:  format = NvKmsSurfaceMemoryFormatX8R8G8B8; break;
		case B_RGBA32: format = NvKmsSurfaceMemoryFormatA8R8G8B8; break;
		default: return B_BAD_VALUE;
	}

	NvKmsRegisterSurfaceParams params {};
	params.request.deviceHandle = fContext.kmsDev->Get();
	// The descriptor is an nvidiactl descriptor carrying an exported RM
	// object (NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD), which is what NVK
	// returns from vkGetMemoryFdKHR. NVKMS imports it in the kernel; the
	// accelerant's own framebuffer is registered the same way
	// (NvKmsBitmap.cpp).
	params.request.useFd = true;
	params.request.planes[0].u.fd = desc->handle;
	params.request.planes[0].offset = desc->offset;
	params.request.widthInPixels = desc->width;
	params.request.heightInPixels = desc->height;
	params.request.format = format;

	// gpu_buffer_desc has no size of the memory object (Renga dropped it
	// from the proposal). NVKMS needs one: it checks that the plane fits,
	// and uses it as the limit of the display's context DMA. Pass what the
	// layout needs. If the object is smaller than that, RM refuses the
	// context DMA and the import fails, so a bad descriptor can't make the
	// display read past the allocation.
	const uint64 bytesPerPixel = 4;
	uint64 pitchBytes;
	uint64 lines = desc->height;
	if (desc->modifier == 0) {
		// Linear.
		pitchBytes = desc->bytes_per_row;
		if (pitchBytes == 0 || pitchBytes % 256 != 0)
			return B_BAD_VALUE;	// NVKMS requires a 256-byte aligned pitch
		params.request.layout = NvKmsSurfaceMemoryLayoutPitch;
		params.request.planes[0].pitch = pitchBytes;
	} else if ((desc->modifier >> kModVendorShift) == kModVendorNvidia
		&& (desc->modifier & 0x10) != 0) {
		// Block-linear. NVKMS wants the pitch in 64-byte GOB columns.
		// Renga's header says bytes_per_row is 0 for non-linear buffers; then
		// the pitch is the width in bytes rounded up to whole GOBs, which is
		// how NVK lays out a single-sample 2D image with one GOB per block
		// in X. A non-zero value is taken as the pitch in bytes.
		// UNVERIFIED: NVK's pitch for every image it may export.
		const uint32 log2GobsY = (uint32)(desc->modifier & 0xf);
		pitchBytes = desc->bytes_per_row != 0 ? desc->bytes_per_row
			: (desc->width * bytesPerPixel + 63) / 64 * 64;
		if (pitchBytes % 64 != 0)
			return B_BAD_VALUE;
		params.request.layout = NvKmsSurfaceMemoryLayoutBlockLinear;
		params.request.log2GobsPerBlockY = log2GobsY;
		params.request.planes[0].pitch = pitchBytes / 64;
		// The last row of blocks is whole even when the image ends inside
		// it: a block is 8 << log2GobsY lines of GOBs.
		const uint64 blockLines = 8ull << log2GobsY;
		lines = (lines + blockLines - 1) / blockLines * blockLines;
	} else {
		return B_BAD_VALUE;
	}
	if (desc->width * bytesPerPixel > pitchBytes)
		return B_BAD_VALUE;
	params.request.planes[0].rmObjectSizeInBytes = desc->offset
		+ lines * pitchBytes;

	std::lock_guard<std::mutex> lock(fLock);
	if (fContext.kms->Control(NVKMS_IOCTL_REGISTER_SURFACE, &params,
			sizeof(params)) < 0)
		return B_ERROR;

	uint32 id = fNextId++;
	if (fNextId == 0)
		fNextId = 1;
	Buffer &buffer = fBuffers[id];
	buffer.surface = NvKmsSurface(*fContext.kmsDev, params.reply.surfaceHandle);
	buffer.desc = *desc;
	buffer.desc.handle = -1;	// not kept: the caller owns it
	*_id = id;
	return B_OK;
}


status_t
NvPresent::ReleaseBuffer(uint32 id)
{
	std::lock_guard<std::mutex> lock(fLock);
	auto it = fBuffers.find(id);
	if (it == fBuffers.end())
		return B_BAD_VALUE;
	// A buffer still on screen, or in a pending flip, is unregistered once
	// the display stops reading it (DropBufferUse).
	it->second.released = true;
	if (it->second.useCount == 0)
		fBuffers.erase(it);
	return B_OK;
}


status_t
NvPresent::SubmitFlip(uint32 head, uint32 plane, NvKmsSurfaceHandle surface,
	const gpu_flip_params *params, bool withNotifier, uint32 notifier)
{
	NvKmsFlipRequestOneHead one {};
	one.sd = 0;
	one.head = head;
	auto &layer = one.flip.layer[plane];
	layer.surface.handle[0] = surface;
	layer.surface.specified = true;

	if (surface != 0 && params != nullptr) {
		const Buffer *buffer = nullptr;
		for (const auto &it : fBuffers) {
			if (it.second.surface.Get() == surface)
				buffer = &it.second;
		}
		uint32 srcWidth = params->source.width;
		uint32 srcHeight = params->source.height;
		if ((srcWidth == 0 || srcHeight == 0) && buffer != nullptr) {
			srcWidth = buffer->desc.width;
			srcHeight = buffer->desc.height;
		}
		layer.sizeIn.val = {.width = (NvU16)srcWidth, .height = (NvU16)srcHeight};
		layer.sizeIn.specified = true;

		if (plane != NVKMS_MAIN_LAYER) {
			uint32 dstWidth = params->destination.width;
			uint32 dstHeight = params->destination.height;
			if (dstWidth == 0 || dstHeight == 0) {
				dstWidth = srcWidth;
				dstHeight = srcHeight;
			}
			layer.sizeOut.val = {.width = (NvU16)dstWidth, .height = (NvU16)dstHeight};
			layer.outputPosition.val = {
				.x = (NvS16)params->destination.x,
				.y = (NvS16)params->destination.y,
			};
			layer.outputPosition.specified = true;
		} else {
			layer.sizeOut.val = layer.sizeIn.val;
		}
		layer.sizeOut.specified = true;

		layer.tearing = (params->flags & B_GPU_FLIP_ASYNC) != 0;
		// 0 lets a flip take effect at the next vblank without waiting for
		// a further one; 1 is the FIFO behaviour of a swap interval of 1.
		layer.minPresentInterval = layer.tearing ? 0 : 1;
		one.flip.allowVrr = (params->flags & B_GPU_FLIP_VRR) != 0;
	}

	if (withNotifier) {
		layer.completionNotifier.val.surface.surfaceHandle = fNotifierSurface.Get();
		layer.completionNotifier.val.surface.format = fNotifierFormat;
		layer.completionNotifier.val.surface.offsetInWords
			= (NvU16)NotifierOffsetInWords(head, plane, notifier);
		layer.completionNotifier.val.awaken = true;
		layer.completionNotifier.specified = true;
	}

	NvKmsFlipParams flip {};
	flip.request.deviceHandle = fContext.kmsDev->Get();
	flip.request.pFlipHead = (NvU64)(addr_t)&one;
	flip.request.numFlipHeads = 1;
	flip.request.commit = true;
	if (fContext.kms->Control(NVKMS_IOCTL_FLIP, &flip, sizeof(flip)) < 0)
		return B_ERROR;
	return B_OK;
}


status_t
NvPresent::Flip(uint32 head, uint32 plane, uint32 id,
	const gpu_flip_params *params)
{
	if (params == nullptr || params->wait_fd < -1)
		return B_BAD_VALUE;
	if ((params->flags & ~(B_GPU_FLIP_VSYNC | B_GPU_FLIP_ASYNC
			| B_GPU_FLIP_VRR)) != 0
		|| ((params->flags & B_GPU_FLIP_VSYNC) != 0
			&& (params->flags & B_GPU_FLIP_ASYNC) != 0))
		return B_BAD_VALUE;
	if (params->source.width > 0xffff || params->source.height > 0xffff
		|| params->destination.width > 0xffff
		|| params->destination.height > 0xffff
		|| params->destination.x < INT16_MIN || params->destination.x > INT16_MAX
		|| params->destination.y < INT16_MIN || params->destination.y > INT16_MAX)
		return B_BAD_VALUE;	// NVKMS sizes are 16-bit
	if (params->source.x != 0 || params->source.y != 0)
		return B_BAD_VALUE;	// NVKMS layers sample from the image origin

	std::lock_guard<std::mutex> lock(fLock);
	PlaneState *state = Plane(head, plane);
	if (state == nullptr)
		return B_BAD_VALUE;
	auto it = fBuffers.find(id);
	if (it == fBuffers.end() || it->second.released)
		return B_BAD_VALUE;
	if (state->pending != 0)
		return B_BUSY;	// one flip in flight per plane, as with KMS

	if (params->wait_fd >= 0) {
		// Wait for rendering in the helper thread, not here: app_server
		// calls flip() from its own event loop and must not block. Our
		// duplicate keeps the descriptor valid however the caller uses its
		// own. NVKMS cannot wait on a descriptor itself (its flips take
		// semaphores in GPU memory, not files), so the flip goes to NVKMS
		// once the descriptor is readable. That costs one helper-thread
		// wakeup of latency, and is the open point "flips waiting on
		// wait_fd" of R-1: to be measured on the RTX 5070.
		if (!fThread.joinable())
			return B_NO_INIT;
		int fd = fcntl(params->wait_fd, F_DUPFD_CLOEXEC, 0);
		if (fd < 0)
			return errno;
		it->second.useCount++;
		state->pending = id;
		state->pendingUserData = params->user_data;
		state->waitFd = fd;
		state->waitParams = *params;
		state->waitParams.wait_fd = -1;
		Wake();
		return B_OK;
	}

	it->second.useCount++;
	state->pending = id;
	state->pendingUserData = params->user_data;
	status_t status = StartFlip(head, plane, *state, *params);
	if (status != B_OK) {
		state->pending = 0;
		DropBufferUse(id);
	}
	return status;
}


// Submit the flip of state.pending to NVKMS. If its completion can't be
// tracked, report it complete now. fLock held.
status_t
NvPresent::StartFlip(uint32 head, uint32 plane, PlaneState &state,
	const gpu_flip_params &params)
{
	auto it = fBuffers.find(state.pending);
	if (it == fBuffers.end())
		return B_BAD_VALUE;

	uint32 notifier = state.nextNotifier;
	if (fHaveNotifiers)
		ResetNotifier(head, plane, notifier);

	status_t status = SubmitFlip(head, plane, it->second.surface.Get(),
		&params, fHaveNotifiers, notifier);
	if (status != B_OK)
		return status;
	state.nextNotifier = (notifier + 1) % kNotifiersPerLayer;

	if (!fHaveNotifiers || !fEventsDeclared) {
		// Without notifiers the flip cannot be tracked: report it as
		// complete now, so the caller does not wait forever. It may tear.
		CompleteFlipNow(head, plane, state, params.user_data);
	}
	return B_OK;
}


// Report the pending flip of a plane as complete, and make its buffer the
// shown one. fLock held.
void
NvPresent::CompleteFlipNow(uint32 head, uint32 plane, PlaneState &state,
	uint64 userData)
{
	gpu_present_event event {};
	event.type = B_GPU_EVENT_FLIP_COMPLETE;
	event.head = head;
	event.plane = plane;
	event.user_data = userData;
	event.time = system_time();
	fEvents.push_back(event);
	if (state.shown != 0)
		DropBufferUse(state.shown);
	state.shown = state.pending;
	state.pending = 0;
	if (fSignalFd >= 0)
		Signal();
}


status_t
NvPresent::DisablePlane(uint32 head, uint32 plane)
{
	std::lock_guard<std::mutex> lock(fLock);
	PlaneState *state = Plane(head, plane);
	if (state == nullptr || plane == NVKMS_MAIN_LAYER)
		return B_BAD_VALUE;	// the main layer always shows a framebuffer
	if (state->waitFd >= 0) {
		// A flip still waiting for rendering is dropped without an event:
		// the caller disabled the plane, so nothing will be shown. The
		// helper thread may be polling this descriptor; it re-checks the
		// plane under fLock before using what it polled.
		close(state->waitFd);
		state->waitFd = -1;
		Wake();
	}
	status_t status = SubmitFlip(head, plane, 0, nullptr, false, 0);
	if (status != B_OK)
		return status;
	// A NULL surface flip completes at the next vblank. The display may read
	// the old buffer until then; NVKMS keeps a reference to the registered
	// surface while it is in use, so unregistering now is safe.
	// UNVERIFIED on hardware.
	if (state->shown != 0)
		DropBufferUse(state->shown);
	if (state->pending != 0)
		DropBufferUse(state->pending);
	state->shown = 0;
	state->pending = 0;
	return B_OK;
}


// repeat true: a B_GPU_EVENT_VBLANK at every vblank of the head from now on.
// repeat false: one at the next vblank, and stop repeating. Renga's header
// does not spell out how repeating stops; this is the reading proposed in
// docs/RENGA-REQUESTS.md (R-1).
status_t
NvPresent::RequestVblank(uint32 head, bool repeat)
{
	if (head >= fContext.headCount)
		return B_BAD_VALUE;
	if (!fThread.joinable())
		return B_NO_INIT;

	std::lock_guard<std::mutex> lock(fLock);
	fRepeatVblank[head] = repeat;
	for (const PendingVblank &it : fPendingVblanks) {
		if (it.head == head)
			return B_OK;	// the next one is already requested
	}
	status_t status = ArmVblank(head);
	if (status == B_OK)
		Wake();
	return status;
}


// Ask NVKMS for one vblank event on a head. fLock held.
status_t
NvPresent::ArmVblank(uint32 head)
{
	// A fresh open of the modeset device, used only as the event's target.
	// It must be opened by this team: the kernel resolves it in the caller's
	// descriptor table (nvkms_get_per_open_data, KmsDevice.cpp).
	int fd = open("/dev/" NVIDIA_MODESET_DEVICE_NAME, O_RDWR | O_CLOEXEC);
	if (fd < 0)
		return errno;

	NvKmsNotifyVblankParams params {};
	params.request.deviceHandle = fContext.kmsDev->Get();
	params.request.dispHandle = fContext.disp;
	params.request.head = head;
	params.request.unicastEvent.fd = fd;
	if (fContext.kms->Control(NVKMS_IOCTL_NOTIFY_VBLANK, &params,
			sizeof(params)) < 0) {
		close(fd);
		return B_ERROR;
	}
	fPendingVblanks.push_back({fd, head});
	return B_OK;
}


int32
NvPresent::EventFd()
{
	return fSignalFd;
}


void
NvPresent::Signal()
{
	eventfd_write(fSignalFd, 1);
}


void
NvPresent::Wake()
{
	if (fWakeFd >= 0)
		eventfd_write(fWakeFd, 1);
}


// Waits on the modeset descriptor, the wake eventfd and every pending vblank
// descriptor; moves what arrives into fEvents and signals fSignalFd.
void
NvPresent::EventThread()
{
	struct WaitingFlip {
		int fd;
		uint32 head;
		uint32 plane;
	};
	std::vector<pollfd> fds;
	std::vector<PendingVblank> pending;
	std::vector<WaitingFlip> waiting;
	while (!fQuit) {
		{
			std::lock_guard<std::mutex> lock(fLock);
			pending = fPendingVblanks;
			waiting.clear();
			for (const auto &it : fPlanes) {
				if (it.second.waitFd >= 0) {
					waiting.push_back({it.second.waitFd, it.first.first,
						it.first.second});
				}
			}
		}
		fds.clear();
		fds.push_back({fContext.kms->Fd(), POLLIN, 0});
		fds.push_back({fWakeFd, POLLIN, 0});
		for (const PendingVblank &it : pending)
			fds.push_back({it.fd, POLLIN, 0});
		for (const WaitingFlip &it : waiting)
			fds.push_back({it.fd, POLLIN, 0});

		if (poll(fds.data(), fds.size(), -1) < 0) {
			if (errno == EINTR)
				continue;
			debug_printf("[!] NvPresent: poll failed (%s)\n", strerror(errno));
			return;
		}

		if ((fds[1].revents & POLLIN) != 0) {
			eventfd_t value;
			eventfd_read(fWakeFd, &value);
		}

		bool signal = false;
		std::lock_guard<std::mutex> lock(fLock);
		if ((fds[0].revents & POLLIN) != 0) {
			size_t before = fEvents.size();
			DrainKmsEvents();
			signal |= fEvents.size() != before;
		}
		for (size_t i = 0; i < pending.size(); i++) {
			if ((fds[i + 2].revents & (POLLIN | POLLERR | POLLHUP)) == 0)
				continue;
			gpu_present_event event {};
			event.type = B_GPU_EVENT_VBLANK;
			event.head = pending[i].head;
			event.time = system_time();
			fEvents.push_back(event);
			signal = true;
			close(pending[i].fd);
			for (auto it = fPendingVblanks.begin(); it != fPendingVblanks.end(); ++it) {
				if (it->fd == pending[i].fd) {
					fPendingVblanks.erase(it);
					break;
				}
			}
			// NVKMS's vblank request is one-shot: re-arm for a repeating
			// head. The event lands at the following vblank as long as this
			// runs within a frame of the last one.
			if (fRepeatVblank[event.head] && ArmVblank(event.head) != B_OK) {
				debug_printf("[!] NvPresent: could not re-arm vblank events "
					"on head %" B_PRIu32 "\n", event.head);
				fRepeatVblank[event.head] = false;
			}
		}
		const size_t waitBase = 2 + pending.size();
		for (size_t i = 0; i < waiting.size(); i++) {
			// POLLERR and POLLHUP count as "rendering done": a fence
			// descriptor that errors will never become readable, and holding
			// the flip forever would hang the plane. POLLNVAL means
			// DisablePlane() closed it.
			if ((fds[waitBase + i].revents & (POLLIN | POLLERR | POLLHUP))
					== 0)
				continue;
			PlaneState *state = Plane(waiting[i].head, waiting[i].plane);
			if (state == nullptr || state->waitFd != waiting[i].fd)
				continue;	// cancelled since the poll began
			close(state->waitFd);
			state->waitFd = -1;
			if (StartFlip(waiting[i].head, waiting[i].plane, *state,
					state->waitParams) != B_OK) {
				// Report it complete so the caller does not wait forever;
				// the previous buffer stays on screen.
				debug_printf("[!] NvPresent: deferred flip on head %" B_PRIu32
					" plane %" B_PRIu32 " failed\n", waiting[i].head,
					waiting[i].plane);
				gpu_present_event done {};
				done.type = B_GPU_EVENT_FLIP_COMPLETE;
				done.head = waiting[i].head;
				done.plane = waiting[i].plane;
				done.user_data = state->waitParams.user_data;
				done.time = system_time();
				fEvents.push_back(done);
				signal = true;
				uint32 id = state->pending;
				state->pending = 0;
				DropBufferUse(id);
			}
		}
		if (signal)
			Signal();
	}
}


// Move NVKMS's queued events into fEvents. A FLIP_OCCURRED event says that
// some flip on (head, layer) with "awaken" completed; the notifier of the
// pending flip says whether it was ours. Other event types (display hot-plug,
// attributes) are not this interface's business and are dropped; the
// accelerant does not otherwise read this queue.
void
NvPresent::DrainKmsEvents()
{
	for (;;) {
		NvKmsGetNextEventParams params {};
		if (fContext.kms->Control(NVKMS_IOCTL_GET_NEXT_EVENT, &params,
				sizeof(params)) < 0 || !params.reply.valid)
			break;
		if (params.reply.event.eventType != NVKMS_EVENT_TYPE_FLIP_OCCURRED)
			continue;

		uint32 head = params.reply.event.u.flipOccurred.head;
		uint32 plane = params.reply.event.u.flipOccurred.layer;
		PlaneState *state = Plane(head, plane);
		if (state == nullptr || state->pending == 0 || state->waitFd >= 0)
			continue;	// nothing of ours on this plane is with NVKMS
		uint32 notifier = (state->nextNotifier + kNotifiersPerLayer - 1)
			% kNotifiersPerLayer;
		if (fHaveNotifiers && !NotifierDone(head, plane, notifier))
			continue;

		gpu_present_event done {};
		done.type = B_GPU_EVENT_FLIP_COMPLETE;
		done.head = head;
		done.plane = plane;
		done.user_data = state->pendingUserData;
		done.time = system_time();
		fEvents.push_back(done);

		if (state->shown != 0)
			DropBufferUse(state->shown);
		state->shown = state->pending;
		state->pending = 0;
	}
}


status_t
NvPresent::NextEvent(gpu_present_event *event)
{
	if (event == nullptr)
		return B_BAD_VALUE;

	std::lock_guard<std::mutex> lock(fLock);
	if (fEvents.empty()) {
		// Clear the eventfd only when the queue is empty, under the lock the
		// helper pushes under, so a signal is never lost: anything pushed
		// after this point signals again.
		eventfd_t value;
		eventfd_read(fSignalFd, &value);
		return B_WOULD_BLOCK;
	}
	*event = fEvents.front();
	fEvents.pop_front();
	return B_OK;
}


status_t
NvPresent::ExportScanout(uint32 head, gpu_buffer_desc *desc)
{
	if (desc == nullptr || head >= fContext.headCount)
		return B_BAD_VALUE;

	std::lock_guard<std::mutex> lock(fLock);
	PlaneState *state = Plane(head, NVKMS_MAIN_LAYER);
	if (state != nullptr && state->shown != 0) {
		// The main layer shows a client's buffer (fullscreen direct
		// scanout). Its descriptor was not kept, so there is nothing to
		// hand out; the capture client has to ask the presenting client.
		// UNVERIFIED design point, see R-2.
		return B_NOT_ALLOWED;
	}

	NvHandle memory = fContext.scanoutMemory(head, desc);
	if (memory == 0)
		return B_ERROR;
	try {
		FileDesc fd = fContext.rmDev->ExportObjectToFd(
			fContext.rmDev->Device().Get(), memory);
		desc->handle_type = B_GPU_BUFFER_FD;
		desc->handle = fd.Detach();
	} catch (const std::system_error &ex) {
		return ToStatus(ex);
	}
	return B_OK;
}


// #pragma mark - hook table


void
NvPresent::SetInstance(NvPresent *present)
{
	sPresent = present;
}


#define PRESENT_CALL(expr) \
	do { \
		if (sPresent == nullptr) \
			return B_NO_INIT; \
		try { \
			return expr; \
		} catch (const std::system_error &ex) { \
			return ToStatus(ex); \
		} catch (...) { \
			return B_ERROR; \
		} \
	} while (0)


static status_t
hook_get_caps(uint32 head, gpu_present_caps *caps)
{
	PRESENT_CALL(sPresent->GetCaps(head, caps));
}

static status_t
hook_import_buffer(const gpu_buffer_desc *desc, uint32 *id)
{
	PRESENT_CALL(sPresent->ImportBuffer(desc, id));
}

static status_t
hook_release_buffer(uint32 id)
{
	PRESENT_CALL(sPresent->ReleaseBuffer(id));
}

static status_t
hook_flip(uint32 head, uint32 plane, uint32 id, const gpu_flip_params *params)
{
	PRESENT_CALL(sPresent->Flip(head, plane, id, params));
}

static status_t
hook_disable_plane(uint32 head, uint32 plane)
{
	PRESENT_CALL(sPresent->DisablePlane(head, plane));
}

static status_t
hook_request_vblank(uint32 head, bool repeat)
{
	PRESENT_CALL(sPresent->RequestVblank(head, repeat));
}

static int
hook_event_fd()
{
	return sPresent != nullptr ? sPresent->EventFd() : -1;
}

static status_t
hook_next_event(gpu_present_event *event)
{
	PRESENT_CALL(sPresent->NextEvent(event));
}

static status_t
hook_export_scanout(uint32 head, gpu_buffer_desc *desc)
{
	PRESENT_CALL(sPresent->ExportScanout(head, desc));
}


const gpu_present_hooks *
NvPresent::Hooks()
{
	static gpu_present_hooks sHooks {};
	if (sPresent == nullptr)
		return nullptr;
	sHooks = {
		.version = B_GPU_PRESENT_VERSION,
		.get_caps = hook_get_caps,
		.import_buffer = hook_import_buffer,
		.release_buffer = hook_release_buffer,
		.flip = hook_flip,
		.disable_plane = hook_disable_plane,
		.request_vblank = hook_request_vblank,
		.event_fd = hook_event_fd,
		.next_event = hook_next_event,
		.export_scanout = hook_export_scanout,
	};
	return &sHooks;
}
