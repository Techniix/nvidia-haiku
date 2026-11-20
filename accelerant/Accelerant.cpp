#include <algorithm>
#include <Accelerant.h>
#include <driver_settings.h>

#include <string.h>
#include <vector>

#include <ErrorUtils.h>
#include <NvRmApi.h>
#include <NvRmDevice.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>
#include <NvKmsSurface.h>

#include "NvUtils.h"
#include "NvKmsBitmap.h"
#include "NvPresent.h"
#include "NvModes.h"
#include "NvMultiHead.h"

// Renga's optional cursor size query, B_GET_CURSOR_MAX_SIZE (F-59, aa1cd0da).
#include <add-ons/graphics/HardwareCursor.h>

#include <memory>

extern "C" {
#include "ctrl/ctrl2080/ctrl2080gpu.h" // NV2080_CTRL_CMD_GPU_GET_NAME_STRING
}

#include "common/edid.h"


static inline status_t ToErrorCode(const std::system_error &ex)
{
	if (ex.code().category() == std::generic_category()) {
		return ex.code().value();
	}
	return B_ERROR;
}


class NvAccelerant {
private:
	static NvAccelerant *sInstance;

	FileDesc fDevFd;
	NvRmApi fRm;
	NvRmDevice fRmDev;
	NvKmsApi fKms;
	NvKmsDevice fKmsDev;
	NvKmsDispHandle fDisp;
	NVDpyId fDpyId;
	NvU32 fHead;

	std::vector<NvKmsMode> fModeList;
	NvKmsMode fCurrentMode {};
	NvKmsDpyAttributeDpmsValue fDpmsState = NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
	NvKmsBitmap fOldFramebuffer, fFramebuffer;

	// Hardware cursor. fCursor is the image app_server last gave us; NVKMS
	// shows it only while fCursorVisible is set. fCursorPos is where
	// app_server puts the hot spot; NVKMS takes the top-left corner.
	// fRetiredCursor is the image fCursor replaced, kept until the next
	// change: freeing a surface makes NVKMS wait for the display channels to
	// go idle, which right after switching images means waiting for that
	// switch to reach the screen (up to a frame) with app_server's lock held.
	NvKmsBitmap fCursor, fRetiredCursor;
	struct {
		int32 x;
		int32 y;
	} fCursorPos {}, fCursorHotSpot {};
	bool fCursorVisible = false;
	// NVKMS refused the image for the current state in a call that can't
	// report errors (show_cursor, or restoring the cursor after a modeset).
	// app_server then believes a cursor is on screen that isn't, so retry
	// from move_cursor: on the next move, then once per kCursorRetryMoves.
	bool fCursorImageStale = false;
	uint32 fCursorRetryCountdown = 0;
	static const uint32 kCursorRetryMoves = 64;

	// GPU presentation (<add-ons/graphics/GpuPresent.h>), created on first request.
	std::unique_ptr<NvPresent> fPresent;
	bool fIsClone = false;

#if QUASAR_HAVE_MULTI_HEAD
	// Several displays (<add-ons/graphics/MultiHead.h>), created on first
	// request; output 0 is this accelerant's classic display.
	std::unique_ptr<NvMultiHead> fMultiHead;
#endif

	void CursorImageChanged();

	NvAccelerant(int devFd);

	NVDpyId FindConnectedDisplay(NVDpyIdList validDpys);
	bool ValidateMode(const NvKmsMode &mode);

	NvKmsSurfaceHandle ShownCursorSurface() const;
	NvKmsMoveCursorCommonParams CursorOrigin() const;
	void ApplyCursorImage(NvKmsSurfaceHandle surface);
	void ApplyCursorPosition();
	void MarkCursorImageStale();
	void SetMode(const display_mode &mode, const NvKmsBitmap &framebuffer, bool withCursor);

public:
	static NvAccelerant *Instance() {return sInstance;}
	~NvAccelerant();

	static void Init(int fd);
	static void Clone(void* data);
	void Uninit();

	ssize_t CloneInfoSize();
	void GetCloneInfo(void* data);
	void GetDeviceInfo(accelerant_device_info* adi);
	uint32 ModeCount();
	void GetModeList(display_mode* modes);
	status_t ProposeMode(display_mode *target, display_mode *low, display_mode *high);
	void SetDisplayMode(display_mode* modeToSet);
	void GetDisplayMode(display_mode* currentMode);
	void GetFrameBufferConfig(frame_buffer_config* frameBuffer);
	void GetPixelClockLimits(display_mode* dm, uint32* low /* in kHz */, uint32* high);

	uint32 DpmsCapabilities();
	uint32 DpmsMode();
	void SetDpmsMode(uint32 dpms_flags);
	void GetPreferredDisplayMode(display_mode* preferredMode);
	void GetMonitorInfo(monitor_info* info);
	void GetEdidInfo(void* info, uint32 size, uint32* _version);

	void MoveCursor(uint16 x, uint16 y);
	void ShowCursor(bool isVisible);
	void SetCursorShape(uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask);
	void SetCursorBitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData);
	uint32 CursorMaxSize();

	const gpu_present_hooks *PresentHooks();
	NvHandle ScanoutMemory(NvU32 head, gpu_buffer_desc *desc);
	uint32 HwHeadOf(uint32 output) const;
	uint32 OutputOf(uint32 hwHead) const;

#if QUASAR_HAVE_MULTI_HEAD
	NvMultiHead *MultiHead();
	void AdoptClassicMode(const NvKmsMode *mode, NvKmsBitmap &&frameBuffer);
#endif
};


NvAccelerant *NvAccelerant::sInstance {};


NvAccelerant::NvAccelerant(int devFd):
	fDevFd(dup(devFd)),
	fRmDev(fRm, 0), // TODO: do not hardcode rmDeviceId
	fKmsDev(fKms, 0)
{
	if (!(fKmsDev.Info().numDisps > 0)) {
		RaiseErrno(ENODEV);
	}
	fDisp = fKmsDev.Info().dispHandles[0];

	NVDpyIdList validDpys;
	{
		NvKmsQueryDispParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DISP, &params, sizeof(params)));

		validDpys = params.reply.validDpys;
	}
	const int32 totalAttempts = 10;
	for (int32 i = 0; i < totalAttempts; i++) {
		fDpyId = FindConnectedDisplay(validDpys);
		if (!nvDpyIdIsInvalid(fDpyId)) {
			break;
		}
		debug_printf("nvidia_gsp: [%" B_PRId32 "/%" B_PRId32 "]: no connected displays\n", i, totalAttempts);
		snooze(100000);
	}
	if (nvDpyIdIsInvalid(fDpyId)) {
		RaiseErrno(ENODEV);
	}

	fHead = 0;

	{
		NvKmsGrabOwnershipParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		CheckErrno(fKms.Control(NVKMS_IOCTL_GRAB_OWNERSHIP, &params, sizeof(params)));
	}

	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {
			.request = {
				.deviceHandle = fKmsDev.Get(),
				.dispHandle = fDisp,
				.dpyId = fDpyId,
				.modeIndex = i,
			},
		};
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params)));
		if (params.reply.end) {
			break;
		}
		if (!ValidateMode(params.reply.mode)) {
			continue;
		}
		fModeList.push_back(params.reply.mode);
	}
}

NVDpyId NvAccelerant::FindConnectedDisplay(NVDpyIdList validDpys)
{
	NVDpyId curDpyId = nvNextDpyIdInDpyIdListUnsorted(nvInvalidDpyId(), validDpys);
	while (!nvDpyIdIsInvalid(curDpyId)) {
		NvKmsQueryDpyDynamicDataParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = curDpyId;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params)));
		if (params.reply.connected || params.reply.edid.valid) {
			break;
		}
		curDpyId = nvNextDpyIdInDpyIdListUnsorted(curDpyId, validDpys);
	}
	return curDpyId;
}

bool NvAccelerant::ValidateMode(const NvKmsMode &mode)
{
	NvKmsValidateModeParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.dispHandle = fDisp;
	params.request.dpyId = fDpyId;
	params.request.mode = mode;
	CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE, &params, sizeof(params)));
	return params.reply.valid;
}

void NvAccelerant::Init(int fd)
{
	debug_printf("NvAccelerant::Init\n");

	sInstance = new NvAccelerant(fd);
}

static const char kCloneInfo[] = "/dev/graphics/nvidia0"; // FIXME

ssize_t NvAccelerant::CloneInfoSize()
{
	debug_printf("NvAccelerant::CloneInfoSize\n");

	return strlen(kCloneInfo) + 1;
}

void NvAccelerant::GetCloneInfo(void* data)
{
	debug_printf("NvAccelerant::GetCloneInfo\n");

	strcpy((char*)data, kCloneInfo);
}

void NvAccelerant::Clone(void* data)
{
	debug_printf("NvAccelerant::Clone\n");

	FileDesc devFd(open((char*)data, O_RDWR | O_CLOEXEC));
	CheckErrno(devFd.Get());

	sInstance = new NvAccelerant(devFd.Get());
	sInstance->fIsClone = true;
}

NvAccelerant::~NvAccelerant()
{
	// The presenter's event thread calls into the multi-head table (hot-plug,
	// head mapping): stop it first.
	fPresent.reset();
#if QUASAR_HAVE_MULTI_HEAD
	fMultiHead.reset();
#endif
}

void NvAccelerant::Uninit()
{
	debug_printf("NvAccelerant::Uninit\n");

	delete sInstance;
	sInstance = nullptr;
}

void NvAccelerant::GetDeviceInfo(accelerant_device_info* adi)
{
	debug_printf("NvAccelerant::GetDeviceInfo\n");

	NV2080_CTRL_GPU_GET_NAME_STRING_PARAMS getNameParams = {
		.gpuNameStringFlags = NV2080_CTRL_GPU_GET_NAME_STRING_FLAGS_TYPE_ASCII,
	};
	fRmDev.Subdevice().Control(NV2080_CTRL_CMD_GPU_GET_NAME_STRING, &getNameParams, sizeof(getNameParams));

	NV2080_CTRL_GPU_GET_SHORT_NAME_STRING_PARAMS getShortNameParams {};
	fRmDev.Subdevice().Control(NV2080_CTRL_CMD_GPU_GET_SHORT_NAME_STRING, &getShortNameParams, sizeof(getShortNameParams));

	adi->version = B_ACCELERANT_VERSION;
	strlcpy(adi->name, (char*)getNameParams.gpuNameString.ascii, sizeof(adi->name));
	strlcpy(adi->chipset, (char*)getShortNameParams.gpuShortNameString, sizeof(adi->chipset));
	strcpy(adi->serial_no, "?");
	adi->memory = 0x20000000; // FIXME
	adi->dac_speed = 0;
}


uint32 NvAccelerant::ModeCount()
{
	debug_printf("NvAccelerant::ModeCount\n");

	return fModeList.size();
}

void NvAccelerant::GetModeList(display_mode* mode)
{
	debug_printf("NvAccelerant::GetModeList\n");

	int32 i = 0;
	for (const auto &nvKmsMode: fModeList) {
		mode[i++] = ToHaikuMode(nvKmsMode);
	}
}

static bool ModeTimingsEqual(const display_timing &a, const display_timing &b)
{
	return
		a.pixel_clock  == b.pixel_clock &&
		a.h_display    == b.h_display &&
		a.h_sync_start == b.h_sync_start &&
		a.h_sync_end   == b.h_sync_end &&
		a.h_total      == b.h_total &&
		a.v_display    == b.v_display &&
		a.v_sync_start == b.v_sync_start &&
		a.v_sync_end   == b.v_sync_end &&
		a.v_total      == b.v_total &&
		a.flags        == b.flags;
}

bool IsDisplayModeWithinBounds(display_mode &mode, const display_mode &low, const display_mode &high)
{
	if (mode.timing.h_display < low.timing.h_display
		|| mode.timing.h_display > high.timing.h_display
		|| mode.timing.h_sync_start < low.timing.h_sync_start
		|| mode.timing.h_sync_start > high.timing.h_sync_start
		|| mode.timing.h_sync_end < low.timing.h_sync_end
		|| mode.timing.h_sync_end > high.timing.h_sync_end
		|| mode.timing.h_total < low.timing.h_total
		|| mode.timing.h_total > high.timing.h_total)
		return false;

	if (mode.timing.v_display < low.timing.v_display
		|| mode.timing.v_display > high.timing.v_display
		|| mode.timing.v_sync_start < low.timing.v_sync_start
		|| mode.timing.v_sync_start > high.timing.v_sync_start
		|| mode.timing.v_sync_end < low.timing.v_sync_end
		|| mode.timing.v_sync_end > high.timing.v_sync_end
		|| mode.timing.v_total < low.timing.v_total
		|| mode.timing.v_total > high.timing.v_total)
		return false;

	if (mode.timing.pixel_clock > high.timing.pixel_clock
		|| mode.timing.pixel_clock < low.timing.pixel_clock)
		return false;

	if (mode.virtual_width > high.virtual_width
		|| mode.virtual_width < low.virtual_width)
		return false;

	if (mode.virtual_height > high.virtual_height
		|| mode.virtual_height < low.virtual_height)
		return false;

	return true;
}

status_t NvAccelerant::ProposeMode(display_mode *target, display_mode *low, display_mode *high)
{
	debug_printf("NvAccelerant::ProposeMode\n");

	for (int32 i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto mode = ToHaikuMode(nvKmsMode);
		if (ModeTimingsEqual(target->timing, mode.timing)) {
			return IsDisplayModeWithinBounds(*target, *low, *high) ? B_OK : B_BAD_VALUE;
		}
	}

	uint32 reqRefreshRate = CalcRefreshRate(target->timing);
	int32 bestCandidateIdx = -1;
	for (int32 i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto timings = ToHaikuModeTimings(nvKmsMode.timings);
		if (
			timings.h_display == target->timing.h_display &&
			timings.v_display == target->timing.v_display &&
			(timings.flags & B_TIMING_INTERLACED) == (target->timing.flags & B_TIMING_INTERLACED)
		) {
			if (bestCandidateIdx < 0 || std::abs((int64)nvKmsMode.timings.RRx1k - (int64)reqRefreshRate) < std::abs((int64)fModeList[bestCandidateIdx].timings.RRx1k - (int64)reqRefreshRate)) {
				bestCandidateIdx = i;
			}
		}
	}
	if (bestCandidateIdx < 0) {
		return B_ERROR;
	}

	target->timing = ToHaikuModeTimings(fModeList[bestCandidateIdx].timings);

	return IsDisplayModeWithinBounds(*target, *low, *high) ? B_OK : B_BAD_VALUE;
}

void NvAccelerant::SetMode(const display_mode &mode, const NvKmsBitmap &framebuffer, bool withCursor)
{
	NvKmsSetModeParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.commit = true;
	params.request.requestedDispsBitMask |= 1U << 0;
	params.request.disp[0].requestedHeadsBitMask |= 1U << 0;
	params.request.disp[0].head[0].dpyIdList = nvAddDpyIdToEmptyDpyIdList(fDpyId);
	params.request.disp[0].head[0].mode = ToNvKmsMode(mode);
	params.request.disp[0].head[0].modeValidationParams.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
	params.request.disp[0].head[0].viewPortOut = {.x = 0, .y = 0, .width = mode.timing.h_display, .height = mode.timing.v_display};
	params.request.disp[0].head[0].viewPortSizeIn = {.width = mode.timing.h_display, .height = mode.timing.v_display};
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].surface.handle[0] = framebuffer.Surface().Get();
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].surface.specified = true;
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeIn.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeIn.specified = true;
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeOut.val = {.width = framebuffer.Width(), .height = framebuffer.Height()};
	params.request.disp[0].head[0].flip.layer[NVKMS_MAIN_LAYER].sizeOut.specified = true;
	if (withCursor) {
		// A modeset clears the cursor (nvClearFlipEvoHwState), so carry it
		// over in the same request.
		params.request.disp[0].head[0].flip.cursor.image.surfaceHandle[NVKMS_LEFT] = ShownCursorSurface();
		params.request.disp[0].head[0].flip.cursor.image.cursorCompParams = CursorCompParams();
		params.request.disp[0].head[0].flip.cursor.imageSpecified = true;
		params.request.disp[0].head[0].flip.cursor.position = CursorOrigin();
		params.request.disp[0].head[0].flip.cursor.positionSpecified = true;
	}

	try {
		CheckErrno(fKms.Control(NVKMS_IOCTL_SET_MODE, &params, sizeof(params)));
	} catch (const std::system_error&) {
		debug_printf("[!] NvAccelerant: SetMode failed\n");
		debug_printf("  status: %d\n", params.reply.status);
		debug_printf("  disp[0].status: %d\n", params.reply.disp[0].status);
		debug_printf("  disp[0].head[0].status: %d\n", params.reply.disp[0].head[0].status);
		throw;
	}
}

void NvAccelerant::SetDisplayMode(display_mode* modeToSet)
{
	debug_printf("NvAccelerant::SetDisplayMode\n");

	if (
		modeToSet->virtual_width != modeToSet->timing.h_display ||
		modeToSet->virtual_height != modeToSet->timing.v_display ||
		modeToSet->h_display_start != 0 ||
		modeToSet->v_display_start != 0
	) {
		RaiseErrno(EINVAL);
	}

	{
		char infoString[NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH] {};
		NvKmsValidateModeParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = fDpyId;
		params.request.modeValidation.overrides = NVKMS_MODE_VALIDATION_NO_RRX1K_CHECK;
		params.request.mode = ToNvKmsMode(*modeToSet);
		params.request.infoStringSize = NVKMS_MODE_VALIDATION_MAX_INFO_STRING_LENGTH;
		params.request.pInfoString = nvKmsPointerToNvU64(infoString);
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE, &params, sizeof(params)));
		if (!params.reply.valid) {
			debug_printf("[!] mode validation failed\n");
			debug_printf("%s", infoString);
			RaiseErrno(EINVAL);
		}
	}

	NvKmsBitmap newFramebuffer(fRmDev, fKmsDev, modeToSet->virtual_width, modeToSet->virtual_height, (color_space)modeToSet->space);

	if (ShownCursorSurface() != 0) {
		try {
			SetMode(*modeToSet, newFramebuffer, true);
			fCursorImageStale = false;
		} catch (const std::system_error&) {
			// Don't let the cursor cost us the mode: retry without it, then
			// put it back the usual way.
			debug_printf("[!] NvAccelerant: SetMode with cursor failed, retrying without\n");
			SetMode(*modeToSet, newFramebuffer, false);
			try {
				ApplyCursorImage(ShownCursorSurface());
				ApplyCursorPosition();
			} catch (const std::system_error &ex) {
				debug_printf("[!] nvidia_gsp: restoring the cursor after a modeset: %s\n", ex.what());
				MarkCursorImageStale();
			}
		}
	} else {
		SetMode(*modeToSet, newFramebuffer, false);
	}
	fCurrentMode = ToNvKmsMode(*modeToSet);

	fOldFramebuffer = std::move(fFramebuffer);
	fFramebuffer = std::move(newFramebuffer);
#if QUASAR_HAVE_MULTI_HEAD
	if (fMultiHead)
		fMultiHead->ClassicModeSet();
#endif
}

#if QUASAR_HAVE_MULTI_HEAD
/*!	set_configuration() (MultiHead.h version 2) set output 0 together with
	the other outputs in one NVKMS request: take over its mode and frame
	buffer, so the classic hooks report what is on screen, and put the
	cursor back (a modeset clears it). mode nullptr: output 0 is off.
*/
void NvAccelerant::AdoptClassicMode(const NvKmsMode *mode, NvKmsBitmap &&frameBuffer)
{
	fOldFramebuffer = std::move(fFramebuffer);
	if (mode == nullptr) {
		fCurrentMode = {};
		fFramebuffer = NvKmsBitmap();
		return;
	}
	fCurrentMode = *mode;
	fFramebuffer = std::move(frameBuffer);
	if (ShownCursorSurface() != 0) {
		try {
			ApplyCursorImage(ShownCursorSurface());
			ApplyCursorPosition();
		} catch (const std::system_error &ex) {
			debug_printf("[!] nvidia_gsp: restoring the cursor after set_configuration: %s\n", ex.what());
			MarkCursorImageStale();
		}
	}
}
#endif

void NvAccelerant::GetDisplayMode(display_mode* currentMode)
{
	debug_printf("NvAccelerant::GetDisplayMode\n");

	if (fCurrentMode.timings.hVisible == 0) {
		RaiseErrno(ENOENT);
	}
	*currentMode = ToHaikuMode(fCurrentMode);
}

void NvAccelerant::GetFrameBufferConfig(frame_buffer_config* frameBuffer)
{
	debug_printf("NvAccelerant::GetFrameBufferConfig\n");

	if (!fFramebuffer.IsSet()) {
		RaiseErrno(ENOENT);
	}
	frameBuffer->frame_buffer = fFramebuffer.Bits();
	frameBuffer->frame_buffer_dma = nullptr;
	frameBuffer->bytes_per_row = fFramebuffer.BytesPerRow();
}

void NvAccelerant::GetPixelClockLimits(display_mode* dm, uint32* low, uint32* high)
{
	debug_printf("NvAccelerant::GetPixelClockLimits\n");

	*low = UINT32_MAX;
	*high = 0;

	for (int32 i = 0; i < fModeList.size(); i++) {
		const auto &nvKmsMode = fModeList[i];
		const auto timing = ToHaikuModeTimings(nvKmsMode.timings);
		if (
			timing.h_display == dm->timing.h_display &&
			timing.v_display == dm->timing.v_display &&
			(timing.flags & B_TIMING_INTERLACED) == (dm->timing.v_display & B_TIMING_INTERLACED)
		) {
			*low = std::min(*low, timing.pixel_clock);
			*high = std::max(*high, timing.pixel_clock);
		}
	}
	if (*low > *high) {
		RaiseErrno(B_ERROR);
	}
}

uint32 NvAccelerant::DpmsCapabilities()
{
	debug_printf("NvAccelerant::DpmsCapabilities\n");

	return B_DPMS_ON | B_DPMS_OFF;
}

uint32 NvAccelerant::DpmsMode()
{
	debug_printf("NvAccelerant::DpmsMode\n");

	switch (fDpmsState) {
		case NV_KMS_DPY_ATTRIBUTE_DPMS_ON:
			return B_DPMS_ON;
		case NV_KMS_DPY_ATTRIBUTE_DPMS_OFF:
			return B_DPMS_OFF;
		default:
			return B_DPMS_OFF;
	}
}

void NvAccelerant::SetDpmsMode(uint32 dpms_flags)
{
	debug_printf("NvAccelerant::SetDpmsMode\n");

	NvS64 value;
	switch (dpms_flags) {
		case B_DPMS_ON:
			value = NV_KMS_DPY_ATTRIBUTE_DPMS_ON;
			break;
		case B_DPMS_OFF:
			value = NV_KMS_DPY_ATTRIBUTE_DPMS_OFF;
			break;
		default:
			RaiseErrno(EINVAL);
	}

	{
		NvKmsSetDpyAttributeParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = fDpyId;
		params.request.attribute = NV_KMS_DPY_ATTRIBUTE_DPMS;
		params.request.value = value;
		CheckErrno(fKms.Control(NVKMS_IOCTL_SET_DPY_ATTRIBUTE, &params, sizeof(params)));
	}
	fDpmsState = (NvKmsDpyAttributeDpmsValue)value;
}

void NvAccelerant::GetPreferredDisplayMode(display_mode* preferredMode)
{
	debug_printf("NvAccelerant::GetPreferredDisplayMode\n");

	for (NvU32 i = 0;; i++) {
		NvKmsValidateModeIndexParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = fDpyId;
		params.request.modeIndex = i;
		CheckErrno(fKms.Control(NVKMS_IOCTL_VALIDATE_MODE_INDEX, &params, sizeof(params)));
		if (params.reply.end)
			break;

		if (params.reply.preferredMode) {
			*preferredMode = ToHaikuMode(params.reply.mode);
			return;
		}
	}

	RaiseErrno(ENOENT);
}

void NvAccelerant::GetMonitorInfo(monitor_info* info)
{
	debug_printf("NvAccelerant::GetMonitorInfo\n");

	RaiseErrno(ENOSYS);
}

void NvAccelerant::GetEdidInfo(void* info, uint32 size, uint32* _version)
{
	debug_printf("NvAccelerant::GetEdidInfo\n");

	if (size < sizeof(struct edid1_info)) {
		RaiseErrno(B_BUFFER_OVERFLOW);
	}

	{
		NvKmsQueryDpyDynamicDataParams params {};
		params.request.deviceHandle = fKmsDev.Get();
		params.request.dispHandle = fDisp;
		params.request.dpyId = fDpyId;
		CheckErrno(fKms.Control(NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA, &params, sizeof(params)));
		if (!params.reply.edid.valid) {
			RaiseErrno(B_ERROR);
		}
		edid_decode((edid1_info*)info, (const edid1_raw*)params.reply.edid.buffer);
		*_version = EDID_VERSION_1;
	}
}


NvKmsSurfaceHandle NvAccelerant::ShownCursorSurface() const
{
	return (fCursorVisible && fCursor.IsSet()) ? fCursor.Surface().Get() : 0;
}

NvKmsMoveCursorCommonParams NvAccelerant::CursorOrigin() const
{
	return {
		.x = (NvS16)(fCursorPos.x - fCursorHotSpot.x),
		.y = (NvS16)(fCursorPos.y - fCursorHotSpot.y),
	};
}

void NvAccelerant::ApplyCursorImage(NvKmsSurfaceHandle surface)
{
	NvKmsSetCursorImageParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.dispHandle = fDisp;
	params.request.head = fHead;
	params.request.common.surfaceHandle[NVKMS_LEFT] = surface;
	params.request.common.cursorCompParams = CursorCompParams();
	CheckErrno(fKms.Control(NVKMS_IOCTL_SET_CURSOR_IMAGE, &params, sizeof(params)));
}

void NvAccelerant::ApplyCursorPosition()
{
	NvKmsMoveCursorParams params {};
	params.request.deviceHandle = fKmsDev.Get();
	params.request.dispHandle = fDisp;
	params.request.head = fHead;
	params.request.common = CursorOrigin();
	CheckErrno(fKms.Control(NVKMS_IOCTL_MOVE_CURSOR, &params, sizeof(params)));
}

void NvAccelerant::MarkCursorImageStale()
{
	fCursorImageStale = true;
	fCursorRetryCountdown = 0;
}

void NvAccelerant::MoveCursor(uint16 x, uint16 y)
{
	fCursorPos.x = x;
	fCursorPos.y = y;
	ApplyCursorPosition();

	if (fCursorImageStale) {
		if (fCursorRetryCountdown > 0) {
			fCursorRetryCountdown--;
			return;
		}
		fCursorRetryCountdown = kCursorRetryMoves;
		ApplyCursorImage(ShownCursorSurface());
		fCursorImageStale = false;
	}
}

void NvAccelerant::ShowCursor(bool isVisible)
{
	if (fCursorVisible == isVisible && !fCursorImageStale) {
		return;
	}
	// Record what app_server wants even if NVKMS refuses it now: modesets
	// and the retry in MoveCursor go by fCursorVisible.
	fCursorVisible = isVisible;
	try {
		ApplyCursorImage(ShownCursorSurface());
	} catch (...) {
		MarkCursorImageStale();
		throw;
	}
	fCursorImageStale = false;
	if (isVisible) {
		// The hot spot may have changed while the cursor was hidden.
		ApplyCursorPosition();
	}
}

void NvAccelerant::SetCursorShape(uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask)
{
	RaiseErrno(ENOSYS);
}

void NvAccelerant::SetCursorBitmap(uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData)
{
	// Anything refused here makes app_server fall back to its software
	// cursor, so refuse rather than show something wrong. app_server's
	// cursors are B_RGBA32; that maps directly to NVKMS's A8R8G8B8, the
	// only cursor format the display engine takes.
	if (colorSpace != B_RGBA32 || bitmapData == NULL || width == 0 || height == 0
		|| bytesPerRow < 4 * (uint32)width) {
		RaiseErrno(EINVAL);
	}

	// The cursor must be square, 32, 64, 128 or 256 pixels. The display
	// engine has no cursor pitch, so rows must be packed, and NvKmsBitmap
	// pads rows to 256 bytes: start at 64 (256 bytes per row).
	const uint32 maxSize = CursorMaxSize();
	uint32 cursorSize = 64;
	while (cursorSize < width || cursorSize < height) {
		cursorSize *= 2;
	}
	if (cursorSize > maxSize) {
		RaiseErrno(EINVAL);
	}

	NvKmsBitmap newCursor(fRmDev, fKmsDev, cursorSize, cursorSize, B_RGBA32);
	if ((uint32)newCursor.BytesPerRow() != 4 * cursorSize) {
		RaiseErrno(EINVAL);
	}
	memset(newCursor.Bits(), 0, newCursor.BitsLength());
	auto dstLine = (uint8*)newCursor.Bits();
	auto srcLine = bitmapData;
	for (uint32 y = 0; y < height; y++) {
		memcpy(dstLine, srcLine, 4 * (size_t)width);
		dstLine += newCursor.BytesPerRow();
		srcLine += bytesPerRow;
	}

	// Free the image retired by the previous change before submitting a new
	// one: the display has long stopped using it, so NVKMS's idle wait on
	// unregistering returns at once.
	fRetiredCursor = NvKmsBitmap();

	// Switch the display over first: if NVKMS refuses the image, the
	// current cursor stays as it was and newCursor is freed on return.
	if (fCursorVisible) {
		ApplyCursorImage(newCursor.Surface().Get());
		fCursorImageStale = false;
	}
	fRetiredCursor = std::move(fCursor);
	fCursor = std::move(newCursor);
	fCursorHotSpot.x = hotX;
	fCursorHotSpot.y = hotY;
	// The other outputs switch before fRetiredCursor is freed (at the next
	// change), so no output ever shows a freed surface.
	CursorImageChanged();
	if (fCursorVisible) {
		ApplyCursorPosition();
	}
}


// The largest cursor set_cursor_bitmap() takes, in both directions: the
// device's limit, and at most 256, the largest of the display engine's cursor
// sizes. Every size up to it is accepted, because a bitmap is padded with
// transparent pixels to the next of 64, 128 and 256 (see SetCursorBitmap()).
// Renga's B_GET_CURSOR_MAX_SIZE reports this, so app_server knows how large a
// pointer with a drag bitmap composited in can stay in hardware.
uint32 NvAccelerant::CursorMaxSize()
{
	uint32 maxSize = fKmsDev.Info().maxCursorSize;
	if (maxSize > 256)
		maxSize = 256;
	// Not a power of two of at least 64: no size is guaranteed to work.
	if (maxSize < 64 || (maxSize & (maxSize - 1)) != 0)
		return 0;
	return maxSize;
}


const gpu_present_hooks *NvAccelerant::PresentHooks()
{
	// Each accelerant instance has its own NVKMS client, and flip events go
	// to the client that flipped. Only app_server's instance presents.
	if (fIsClone)
		return nullptr;
	if (!fPresent) {
		NvPresent::Context context {
			.rmDev = &fRmDev,
			.kms = &fKms,
			.kmsDev = &fKmsDev,
			.disp = fDisp,
			.headCount = fKmsDev.Info().numHeads,
			.scanoutMemory = [](NvU32 head, gpu_buffer_desc *desc) {
				return NvAccelerant::Instance()->ScanoutMemory(head, desc);
			},
			.hwHeadOf = [](uint32 output) {
				return NvAccelerant::Instance()->HwHeadOf(output);
			},
			.outputOf = [](uint32 hwHead) {
				return NvAccelerant::Instance()->OutputOf(hwHead);
			},
			.hotplug = []() {
#if QUASAR_HAVE_MULTI_HEAD
				NvAccelerant *accelerant = NvAccelerant::Instance();
				if (accelerant != nullptr && accelerant->fMultiHead)
					accelerant->fMultiHead->Hotplug();
#endif
			},
		};
		fPresent = std::make_unique<NvPresent>(context);
		NvPresent::SetInstance(fPresent.get());
	}
	return NvPresent::Hooks();
}

// The memory a hardware head scans out: the classic frame buffer on fHead,
// or the frame buffer of the multi-head output on another head.
NvHandle NvAccelerant::ScanoutMemory(NvU32 head, gpu_buffer_desc *desc)
{
	if (head != fHead) {
#if QUASAR_HAVE_MULTI_HEAD
		if (fMultiHead)
			return fMultiHead->ScanoutMemory(fMultiHead->OutputOf(head), desc);
#endif
		return 0;
	}
	if (!fFramebuffer.IsSet())
		return 0;
	*desc = {};
	desc->width = fFramebuffer.Width();
	desc->height = fFramebuffer.Height();
	desc->bytes_per_row = fFramebuffer.BytesPerRow();
	desc->format = fFramebuffer.ColorSpace();
	desc->modifier = 0;	// linear
	desc->offset = 0;
	return fFramebuffer.Memory().Get();
}


// Output numbers (MultiHead.h) <-> hardware heads. Without the multi-head
// table only output 0, the classic display on fHead, exists.
uint32 NvAccelerant::HwHeadOf(uint32 output) const
{
#if QUASAR_HAVE_MULTI_HEAD
	if (fMultiHead)
		return fMultiHead->HwHeadOf(output);
#endif
	return output == 0 ? fHead : kNvNoHead;
}

uint32 NvAccelerant::OutputOf(uint32 hwHead) const
{
#if QUASAR_HAVE_MULTI_HEAD
	if (fMultiHead)
		return fMultiHead->OutputOf(hwHead);
#endif
	return hwHead == fHead ? 0 : kNvNoHead;
}

#if QUASAR_HAVE_MULTI_HEAD
NvMultiHead *NvAccelerant::MultiHead()
{
	// Like GPU presentation, only app_server's instance drives displays.
	if (fIsClone)
		return nullptr;
	if (!fMultiHead) {
		fMultiHead = std::make_unique<NvMultiHead>(NvMultiHead::Context {
			.rmDev = &fRmDev,
			.kms = &fKms,
			.kmsDev = &fKmsDev,
			.disp = fDisp,
			.classicDpy = fDpyId,
			.classicHwHead = fHead,
			.adoptClassic = [](const NvKmsMode *mode, NvKmsBitmap &&frameBuffer) {
				NvAccelerant::Instance()->AdoptClassicMode(mode, std::move(frameBuffer));
			},
			.classicFrameBuffer = []() -> const NvKmsBitmap * {
				NvAccelerant *accelerant = NvAccelerant::Instance();
				return accelerant->fFramebuffer.IsSet() ? &accelerant->fFramebuffer : nullptr;
			},
		});
		CursorImageChanged();
		// Hot-plug events arrive on the presenter's NVKMS event thread.
		PresentHooks();
	}
	return fMultiHead.get();
}

#endif	// QUASAR_HAVE_MULTI_HEAD

// The cursor image is the classic hooks', shared by every output.
void NvAccelerant::CursorImageChanged()
{
#if QUASAR_HAVE_MULTI_HEAD
	if (!fMultiHead)
		return;
	fMultiHead->SetCursorImage(fCursor.IsSet() ? fCursor.Surface().Get() : 0,
		fCursorHotSpot.x, fCursorHotSpot.y);
#endif
}


// Optional timing of the cursor hooks, for checking that they never stall
// app_server (which holds its exclusive drawing lock across them). Enable
// with "cursor_timing true" in the settings file below; the syslog then gets
// a line per 4096 moves and per 64 other calls.
struct CursorTiming {
	const char *name;
	uint32 reportEvery;
	uint32 count = 0;
	bigtime_t total = 0;
	bigtime_t max = 0;

	void Add(bigtime_t t)
	{
		count++;
		total += t;
		if (t > max)
			max = t;
		if (count == reportEvery) {
			debug_printf("nvidia_gsp: %s: %" B_PRIu32 " calls, average %" B_PRIdBIGTIME
				" us, max %" B_PRIdBIGTIME " us\n", name, count, total / count, max);
			count = 0;
			total = 0;
			max = 0;
		}
	}
};

static bool sCursorTiming = false;
static CursorTiming sMoveTiming {"move_cursor", 4096};
static CursorTiming sShowTiming {"show_cursor", 64};
static CursorTiming sBitmapTiming {"set_cursor_bitmap", 64};

class CursorTimer {
	CursorTiming &fTiming;
	bigtime_t fStart;
public:
	CursorTimer(CursorTiming &timing): fTiming(timing), fStart(sCursorTiming ? system_time() : 0) {}
	~CursorTimer() { if (sCursorTiming) fTiming.Add(system_time() - fStart); }
};

// move_cursor and show_cursor can't report errors and run on every mouse
// move: log the first failure and then one line per 1024.
static void LogCursorError(const char *hook, const char *what)
{
	static uint32 sErrors = 0;
	if (sErrors++ % 1024 == 0) {
		debug_printf("[!] nvidia_gsp: %s: %s (%" B_PRIu32 " cursor errors so far)\n", hook, what, sErrors);
	}
}

static bool HardwareCursorEnabled()
{
	// Opt-out, in ~/config/settings/kernel/drivers/nvidia_gsp:
	//   hardware_cursor false
	// Without the file or the line the hardware cursor is used.
	static int8 sEnabled = -1;
	if (sEnabled < 0) {
		void *handle = load_driver_settings("nvidia_gsp");
		sEnabled = get_driver_boolean_parameter(handle, "hardware_cursor", true, true) ? 1 : 0;
		sCursorTiming = get_driver_boolean_parameter(handle, "cursor_timing", false, true);
		unload_driver_settings(handle);
		debug_printf("nvidia_gsp: hardware cursor %s%s\n", sEnabled ? "enabled" : "disabled by settings",
			sEnabled && sCursorTiming ? ", timing on" : "");
	}
	return sEnabled != 0;
}


#if QUASAR_HAVE_MULTI_HEAD
// #pragma mark - multi-head table


// Renga's <add-ons/graphics/MultiHead.h>. Output 0 is the classic display:
// its mode, frame buffer, DPMS and cursor go through the classic code, so
// the classic hooks and the table always agree. The other outputs are
// NvMultiHead's.
#define MULTI_CALL(expr) \
	do { \
		try { \
			return (expr); \
		} catch (const std::system_error &ex) { \
			debug_printf("[!] nvidia_gsp: multi-head: %s\n", ex.what()); \
			return ToErrorCode(ex); \
		} catch (...) { \
			return (status_t)B_ERROR; \
		} \
	} while (0)

static NvMultiHead *Multi()
{
	return NvAccelerant::Instance()->MultiHead();
}

template<typename F>
static status_t ClassicCall(F f)
{
	f();
	return B_OK;
}

static uint32 mh_count_outputs()
{
	try {
		return Multi()->Count();
	} catch (...) {
		return 1;
	}
}

static status_t mh_get_output_info(uint32 head, multi_head_info *info)
{
	MULTI_CALL(Multi()->GetInfo(head, info));
}

static uint32 mh_mode_count(uint32 head)
{
	try {
		if (head == 0)
			return NvAccelerant::Instance()->ModeCount();
		return Multi()->ModeCount(head);
	} catch (...) {
		return 0;
	}
}

static status_t mh_get_mode_list(uint32 head, display_mode *modes)
{
	if (head == 0)
		MULTI_CALL(ClassicCall([&] {NvAccelerant::Instance()->GetModeList(modes);}));
	MULTI_CALL(Multi()->GetModeList(head, modes));
}

static status_t mh_set_output_mode(uint32 head, const display_mode *mode)
{
	if (head == 0) {
		// The classic display has no "off": app_server uses DPMS for it.
		if (mode == nullptr)
			return B_NOT_SUPPORTED;
		display_mode copy = *mode;
		MULTI_CALL(ClassicCall([&] {NvAccelerant::Instance()->SetDisplayMode(&copy);}));
	}
	MULTI_CALL(Multi()->SetMode(head, mode));
}

static status_t mh_get_output_mode(uint32 head, display_mode *mode)
{
	if (head == 0)
		MULTI_CALL(ClassicCall([&] {NvAccelerant::Instance()->GetDisplayMode(mode);}));
	MULTI_CALL(Multi()->GetMode(head, mode));
}

static status_t mh_get_frame_buffer_config(uint32 head, frame_buffer_config *config)
{
	if (head == 0)
		MULTI_CALL(ClassicCall([&] {NvAccelerant::Instance()->GetFrameBufferConfig(config);}));
	MULTI_CALL(Multi()->GetFrameBufferConfig(head, config));
}

static status_t mh_set_output_dpms(uint32 head, uint32 dpmsFlags)
{
	if (head == 0)
		MULTI_CALL(ClassicCall([&] {NvAccelerant::Instance()->SetDpmsMode(dpmsFlags);}));
	MULTI_CALL(Multi()->SetDpms(head, dpmsFlags));
}

static status_t mh_move_output_cursor(uint32 head, int32 x, int32 y)
{
	if (!HardwareCursorEnabled())
		return B_NOT_SUPPORTED;
	// The classic hook takes uint16; outside output 0 the cursor is off it.
	if (head == 0) {
		uint16 cx = (uint16)std::clamp<int32>(x, 0, UINT16_MAX);
		uint16 cy = (uint16)std::clamp<int32>(y, 0, UINT16_MAX);
		MULTI_CALL(ClassicCall([&] {NvAccelerant::Instance()->MoveCursor(cx, cy);}));
	}
	MULTI_CALL(Multi()->MoveCursor(head, x, y));
}

static status_t mh_show_output_cursor(uint32 head, bool visible)
{
	if (!HardwareCursorEnabled())
		return B_NOT_SUPPORTED;
	if (head == 0)
		MULTI_CALL(ClassicCall([&] {NvAccelerant::Instance()->ShowCursor(visible);}));
	MULTI_CALL(Multi()->ShowCursor(head, visible));
}

static status_t mh_get_output_edid(uint32 head, void *data, size_t *size)
{
	MULTI_CALL(Multi()->GetEdid(head, data, size));
}

static int mh_event_fd()
{
	try {
		return Multi()->EventFd();
	} catch (...) {
		return -1;
	}
}

static uint32 mh_count_hardware_heads()
{
	try {
		return Multi()->HardwareHeadCount();
	} catch (...) {
		return 1;
	}
}

static status_t mh_set_configuration(const multi_head_config *configs, uint32 count,
	uint32 flags)
{
	MULTI_CALL(Multi()->SetConfiguration(configs, count, flags));
}

static const multi_head_hooks *MultiHeadHooks()
{
	static const multi_head_hooks sHooks = {
		.version = 2,
		.count_outputs = mh_count_outputs,
		.get_output_info = mh_get_output_info,
		.mode_count = mh_mode_count,
		.get_mode_list = mh_get_mode_list,
		.set_output_mode = mh_set_output_mode,
		.get_output_mode = mh_get_output_mode,
		.get_frame_buffer_config = mh_get_frame_buffer_config,
		.set_output_dpms = mh_set_output_dpms,
		.move_output_cursor = mh_move_output_cursor,
		.show_output_cursor = mh_show_output_cursor,
		.get_output_edid = mh_get_output_edid,
		.event_fd = mh_event_fd,
		.count_hardware_heads = mh_count_hardware_heads,
		.set_configuration = mh_set_configuration,
	};
	return &sHooks;
}
#endif	// QUASAR_HAVE_MULTI_HEAD


_EXPORT void *get_accelerant_hook(uint32 feature, void *data)
{
	switch (feature) {
		case B_INIT_ACCELERANT: {
			init_accelerant fn = [](int fd) {
				try {
					NvAccelerant::Init(fd);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
		case B_ACCELERANT_CLONE_INFO_SIZE: {
			accelerant_clone_info_size fn = []() {
				return NvAccelerant::Instance()->CloneInfoSize();
			};
			return (void*)fn;
		}
		case B_GET_ACCELERANT_CLONE_INFO: {
			get_accelerant_clone_info fn = [](void* data) {
				return NvAccelerant::Instance()->GetCloneInfo(data);
			};
			return (void*)fn;
		}
		case B_CLONE_ACCELERANT: {
			clone_accelerant fn = [](void *data) {
				try {
					NvAccelerant::Clone(data);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
		case B_UNINIT_ACCELERANT: {
			uninit_accelerant fn = []() {
				try {
					NvAccelerant::Instance()->Uninit();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
				}
			};
			return (void*)fn;
		}
		case B_GET_ACCELERANT_DEVICE_INFO: {
			get_accelerant_device_info fn = [](accelerant_device_info* adi) {
				try {
					NvAccelerant::Instance()->GetDeviceInfo(adi);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}

		case B_ACCELERANT_MODE_COUNT: {
			accelerant_mode_count fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->ModeCount();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return 0;
				}
			};
			return (void*)fn;
		}
		case B_GET_MODE_LIST: {
			get_mode_list fn = [](display_mode* modes) {
				try {
					NvAccelerant::Instance()->GetModeList(modes);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
		case B_PROPOSE_DISPLAY_MODE: {
			propose_display_mode fn = [](display_mode *target, display_mode *low, display_mode *high) {
				try {
					return NvAccelerant::Instance()->ProposeMode(target, low, high);
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
		case B_SET_DISPLAY_MODE: {
			set_display_mode fn = [](display_mode* modeToSet) {
				try {
					NvAccelerant::Instance()->SetDisplayMode(modeToSet);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
		case B_GET_DISPLAY_MODE: {
			get_display_mode fn = [](display_mode* currentMode) {
				try {
					NvAccelerant::Instance()->GetDisplayMode(currentMode);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
		case B_GET_FRAME_BUFFER_CONFIG: {
			get_frame_buffer_config fn = [](frame_buffer_config* frameBuffer) {
				try {
					NvAccelerant::Instance()->GetFrameBufferConfig(frameBuffer);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
		case B_GET_PIXEL_CLOCK_LIMITS: {
			get_pixel_clock_limits fn = [](display_mode* dm, uint32* low, uint32* high) {
				try {
					NvAccelerant::Instance()->GetPixelClockLimits(dm, low, high);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}

		case B_DPMS_CAPABILITIES: {
			dpms_capabilities fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->DpmsCapabilities();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return 0;
				}
			};
			return (void*)fn;
		}
		case B_DPMS_MODE: {
			dpms_mode fn = []() -> uint32 {
				try {
					return NvAccelerant::Instance()->DpmsMode();
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return B_DPMS_ON;
				}
			};
			return (void*)fn;
		}
		case B_SET_DPMS_MODE: {
			set_dpms_mode fn = [](uint32 dpms_flags) {
				try {
					NvAccelerant::Instance()->SetDpmsMode(dpms_flags);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
		case B_GET_PREFERRED_DISPLAY_MODE: {
			get_preferred_display_mode fn = [](display_mode* preferredMode) {
				try {
					NvAccelerant::Instance()->GetPreferredDisplayMode(preferredMode);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
#if 0
		case B_GET_MONITOR_INFO: {
			get_monitor_info fn = [](monitor_info* info) {
				try {
					NvAccelerant::Instance()->GetMonitorInfo(info);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
#endif
		case B_GET_EDID_INFO: {
			get_edid_info fn = [](void* info, uint32 size, uint32* _version) {
				try {
					NvAccelerant::Instance()->GetEdidInfo(info, size, _version);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}

		case B_MOVE_CURSOR: {
			if (!HardwareCursorEnabled()) {
				return nullptr;
			}
			move_cursor fn = [](uint16 x, uint16 y) {
				CursorTimer timer(sMoveTiming);
				try {
					NvAccelerant::Instance()->MoveCursor(x, y);
				} catch (const std::system_error &ex) {
					LogCursorError("move_cursor", ex.what());
				} catch (...) {
					LogCursorError("move_cursor", "unexpected exception");
				}
			};
			return (void*)fn;
		}
#if 0
		// Monochrome cursors: not implemented. app_server only uses this when
		// B_SET_CURSOR_BITMAP refuses a cursor, and then falls back to its
		// software cursor anyway.
		case B_SET_CURSOR_SHAPE: {
			set_cursor_shape fn = [](uint16 width, uint16 height, uint16 hotX, uint16 hotY, const uint8* andMask, const uint8* xorMask) {
				try {
					NvAccelerant::Instance()->SetCursorShape(width, height, hotX, hotY, andMask, xorMask);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					debug_printf("[!] nvidia_gsp: unexpected exception\n");
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}
#endif
		case B_SHOW_CURSOR: {
			if (!HardwareCursorEnabled()) {
				return nullptr;
			}
			show_cursor fn = [](bool isVisible) {
				CursorTimer timer(sShowTiming);
				try {
					NvAccelerant::Instance()->ShowCursor(isVisible);
				} catch (const std::system_error &ex) {
					LogCursorError("show_cursor", ex.what());
				} catch (...) {
					LogCursorError("show_cursor", "unexpected exception");
				}
			};
			return (void*)fn;
		}
		case B_SET_CURSOR_BITMAP: {
			if (!HardwareCursorEnabled()) {
				return nullptr;
			}
			set_cursor_bitmap fn = [](uint16 width, uint16 height, uint16 hotX, uint16 hotY, color_space colorSpace, uint16 bytesPerRow, const uint8* bitmapData) {
				CursorTimer timer(sBitmapTiming);
				try {
					NvAccelerant::Instance()->SetCursorBitmap(width, height, hotX, hotY, colorSpace, bytesPerRow, bitmapData);
					return B_OK;
				} catch (const std::system_error &ex) {
					debug_printf("[!] nvidia_gsp: %s\n", ex.what());
					return ToErrorCode(ex);
				} catch (...) {
					// e.g. std::bad_alloc; never let it unwind into app_server
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}

		// Renga (F-59): the largest cursor app_server may hand to
		// set_cursor_bitmap(). While something is dragged, app_server
		// composites the pointer and the drag bitmap into one cursor image;
		// with this hook it never offers one larger than we take, so it
		// doesn't need a refused call to fall back to its software cursor.
		// Only B_RGBA32, the one cursor format set_cursor_bitmap() takes.
		case B_GET_CURSOR_MAX_SIZE: {
			if (!HardwareCursorEnabled()) {
				return nullptr;
			}
			get_cursor_max_size fn = [](color_space space, uint16* maxWidth, uint16* maxHeight) {
				if (maxWidth == NULL || maxHeight == NULL)
					return (status_t)B_BAD_VALUE;
				if (space != B_RGBA32)
					return (status_t)B_NOT_SUPPORTED;
				try {
					uint32 size = NvAccelerant::Instance()->CursorMaxSize();
					if (size == 0)
						return (status_t)B_NOT_SUPPORTED;
					*maxWidth = (uint16)size;
					*maxHeight = (uint16)size;
					return (status_t)B_OK;
				} catch (...) {
					return (status_t)B_ERROR;
				}
			};
			return (void*)fn;
		}

		// Renga GPU presentation (provisional, docs/RENGA-REQUESTS.md R-1).
		// Not a function pointer like the hooks above: the table itself.
		// Only the accelerant's owner gets it; clones return NULL, because
		// flips and their events belong to one client.
		case B_GPU_PRESENT_HOOKS: {
			try {
				return (void*)NvAccelerant::Instance()->PresentHooks();
			} catch (const std::system_error &ex) {
				debug_printf("[!] nvidia_gsp: GPU presentation unavailable: %s\n", ex.what());
				return nullptr;
			}
		}

#if QUASAR_HAVE_MULTI_HEAD
		// Renga's several-displays table (provisional, MultiHead.h; R-19).
		// The table itself; NULL for clones, which drive no display.
		case B_MULTI_HEAD_HOOKS: {
			try {
				if (NvAccelerant::Instance()->MultiHead() == nullptr)
					return nullptr;
				return (void*)MultiHeadHooks();
			} catch (const std::system_error &ex) {
				debug_printf("[!] nvidia_gsp: multi-head table unavailable: %s\n", ex.what());
				return nullptr;
			}
		}
#endif

		default:
			return nullptr;
	}
}
