#pragma once

/*
 * Several displays on one GPU: the accelerant half of Renga's
 * <add-ons/graphics/MultiHead.h> (version 2, Renga a55e2329, which is
 * R-19 as accepted), on NVKMS.
 *
 * Numbering. The table's numbers are display outputs (NVKMS dpys):
 *  - output 0 is the display the classic hooks drive (the accelerant's
 *    first connected dpy, on hardware head 0). Everything about output 0
 *    except its description is answered by the classic code in
 *    Accelerant.cpp, so the two never disagree;
 *  - outputs 1.. are the GPU's other dpys, in NVKMS's order. A connector
 *    keeps its number while the accelerant is loaded.
 * Hardware heads are chosen when an output is enabled: the lowest head the
 * dpy can use (NVKMS's headMask) that no other enabled output holds. The
 * classic head (0) is reserved for output 0, so the classic hooks can always
 * turn output 0 back on. <GpuPresent.h>'s head arguments are output
 * numbers; NvPresent maps them through HwHeadOf()/OutputOf().
 *
 * set_configuration() (version 2) is one NVKMS SET_MODE over every hardware
 * head: NVKMS validates the whole configuration (shared display bandwidth,
 * IMP) and applies it or nothing. TEST_ONLY is the same request with commit
 * false and no surfaces. Output 0 takes part like any other output; its new
 * frame buffer and mode are handed to the classic code (Context::adopt-
 * Classic) so the classic hooks stay in step. Mirrors scan out their
 * source's surface on their own head.
 *
 * Not yet handled: DisplayPort MST displays behind a connector (NVKMS's
 * dynamic dpys) get no number, and B_OUTPUT_CONFIG_ALLOW_VRR is accepted
 * but VRR is not yet enabled at mode-set time (R-7).
 *
 * Threads: every hook runs in app_server's thread that called it. HwHeadOf()
 * and OutputOf() are also called from NvPresent's event thread and read only
 * atomics. Hotplug() is called from that thread and only writes the
 * eventfd.
 */

#include <stdint.h>

// No head / no output, in the output <-> hardware head mapping.
static const uint32_t kNvNoHead = ~(uint32_t)0;

// <add-ons/graphics/MultiHead.h> is in Renga's development headers from the
// first image after 2026-11-20. The image the current hardware tests use
// (143b8c30) predates it, so until renga-setup.sh can require a newer image
// the table is built only when the header is there (docs/RENGA-STATUS.md,
// transitional code). Without it the accelerant offers no B_MULTI_HEAD_HOOKS,
// exactly as an accelerant that does not implement the table.
//
// Quasar implements version 2 (set_configuration()). Image 2026.11.20-batch4
// carries version 1, which Quasar no longer builds against: the table is
// compiled only with version 2 or later.
#if __has_include(<add-ons/graphics/MultiHead.h>)
#	include <Accelerant.h>
#	include <add-ons/graphics/MultiHead.h>
#	if B_MULTI_HEAD_VERSION >= 2
#		define QUASAR_HAVE_MULTI_HEAD 1
#	else
#		define QUASAR_HAVE_MULTI_HEAD 0
#	endif
#else
#	define QUASAR_HAVE_MULTI_HEAD 0
#endif

#if QUASAR_HAVE_MULTI_HEAD

#include <atomic>
#include <memory>
#include <vector>

#include <Accelerant.h>
#include <add-ons/graphics/MultiHead.h>
#include <add-ons/graphics/GpuPresent.h>	// gpu_buffer_desc

#include <NvRmDevice.h>
#include <NvKmsApi.h>
#include <NvKmsDevice.h>

#include "NvKmsBitmap.h"


class NvMultiHead {
public:
	struct Context {
		NvRmDevice *rmDev;
		NvKmsApi *kms;
		NvKmsDevice *kmsDev;
		NvKmsDispHandle disp;
		NVDpyId classicDpy;		// output 0
		NvU32 classicHwHead;	// its hardware head (0)
		// set_configuration() changed output 0: mode nullptr turned it off,
		// otherwise the classic code adopts mode and frame buffer (and puts
		// its cursor back, which a modeset clears).
		void (*adoptClassic)(const NvKmsMode *mode, NvKmsBitmap &&frameBuffer);
		// The classic code's current frame buffer, for mirrors of output 0
		// and for set_configuration() keeping output 0's buffer.
		const NvKmsBitmap *(*classicFrameBuffer)();
	};

	static const uint32 kNoHead = kNvNoHead;

	explicit NvMultiHead(const Context &context);
	~NvMultiHead();

	NvMultiHead(const NvMultiHead &) = delete;
	NvMultiHead &operator=(const NvMultiHead &) = delete;

	uint32 Count() const {return (uint32)fOutputs.size();}
	uint32 HardwareHeadCount() const;
	// The classic hooks set a mode: output 0 is on the classic head again.
	void ClassicModeSet();

	// Version 2: all outputs at once, all or nothing.
	status_t SetConfiguration(const multi_head_config *configs, uint32 count,
		uint32 flags);

	// Every output, 0 included.
	status_t GetInfo(uint32 output, multi_head_info *info);
	status_t GetEdid(uint32 output, void *data, size_t *_size);

	// Outputs 1.. only; output 0 goes to the classic hooks.
	uint32 ModeCount(uint32 output);
	status_t GetModeList(uint32 output, display_mode *modes);
	status_t SetMode(uint32 output, const display_mode *mode);
	status_t GetMode(uint32 output, display_mode *mode);
	status_t GetFrameBufferConfig(uint32 output, frame_buffer_config *config);
	status_t SetDpms(uint32 output, uint32 dpmsFlags);
	status_t MoveCursor(uint32 output, int32 x, int32 y);
	status_t ShowCursor(uint32 output, bool visible);

	// The classic hooks changed the cursor image or hot spot: show the new
	// one on every output 1.. that shows a cursor. surface 0: no image.
	void SetCursorImage(NvKmsSurfaceHandle surface, int32 hotX, int32 hotY);

	// The memory output 1.. scans out, for NvPresent's export_scanout (0:
	// none). desc is filled as for the classic frame buffer.
	NvHandle ScanoutMemory(uint32 output, gpu_buffer_desc *desc);

	// Output <-> hardware head, for <GpuPresent.h>. kNoHead when the output
	// is off (or the head drives no output).
	uint32 HwHeadOf(uint32 output) const;
	uint32 OutputOf(uint32 hwHead) const;

	// Hot-plug: event_fd() is readable after NVKMS reported a dpy change.
	int EventFd() const {return fEventFd;}
	void Hotplug();

private:
	struct Output {
		NVDpyId dpy;
		NvU32 headMask = 0;			// heads that can drive it
		std::atomic<uint32> hwHead {kNoHead};
		std::vector<NvKmsMode> modes;
		std::atomic<bool> modesValid {false};	// cleared by Hotplug()
		NvKmsMode mode {};			// hVisible 0: off
		NvKmsBitmap frameBuffer, oldFrameBuffer;
		uint32 mirrorOf = B_NO_SOURCE_OUTPUT;	// scans out that output's buffer
		bool dpmsOff = false;
		bool cursorVisible = false;
		int32 cursorX = 0, cursorY = 0;
	};

	Output *Extra(uint32 output);	// outputs 1.., else throws EINVAL
	void LoadModes(Output &out);
	bool ValidateMode(const Output &out, const NvKmsMode &mode);
	uint32 PickHwHead(const Output &out) const;
	const NvKmsBitmap *FrameBufferOf(uint32 output) const;
	void SubmitMode(Output &out, uint32 hwHead, const NvKmsMode *mode,
		const NvKmsBitmap *frameBuffer);
	void ApplyCursor(Output &out);
	void QueryDynamic(NVDpyId dpy, NvKmsQueryDpyDynamicDataReply &reply);

	Context fContext;
	std::vector<std::unique_ptr<Output>> fOutputs;
	int fEventFd = -1;

	NvKmsSurfaceHandle fCursorSurface = 0;
	int32 fCursorHotX = 0, fCursorHotY = 0;
};

#endif	// QUASAR_HAVE_MULTI_HEAD
