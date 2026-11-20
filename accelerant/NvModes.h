#pragma once

/*
 * Conversions between Haiku's display_mode and NVKMS's mode timings, shared
 * by the classic hooks (Accelerant.cpp) and the multi-head table
 * (NvMultiHead.cpp). Moved unchanged from Accelerant.cpp.
 */

#include <Accelerant.h>

extern "C" {
#include "nvkms-api.h"
}


static inline uint32 CalcRefreshRate(const display_timing &haikuModeTimings)
{
	uint64 multiplier = (B_TIMING_INTERLACED & haikuModeTimings.flags) != 0 ? 2000000LL : 1000000LL;
	return (uint32)(multiplier * haikuModeTimings.pixel_clock / ((uint64)haikuModeTimings.h_total * haikuModeTimings.v_total));
}

static inline display_timing ToHaikuModeTimings(const NvModeTimings &nvKmsModeTimings) {
	display_timing haikuModeTimings {
		.pixel_clock  = nvKmsModeTimings.pixelClockHz / 1000,
		.h_display    = nvKmsModeTimings.hVisible,
		.h_sync_start = nvKmsModeTimings.hSyncStart,
		.h_sync_end   = nvKmsModeTimings.hSyncEnd,
		.h_total      = nvKmsModeTimings.hTotal,
		.v_display    = nvKmsModeTimings.vVisible,
		.v_sync_start = nvKmsModeTimings.vSyncStart,
		.v_sync_end   = nvKmsModeTimings.vSyncEnd,
		.v_total      = nvKmsModeTimings.vTotal,
		.flags        =
			(nvKmsModeTimings.interlaced ? B_TIMING_INTERLACED : 0) |
			(nvKmsModeTimings.hSyncPos ? B_POSITIVE_HSYNC : 0) |
			(nvKmsModeTimings.vSyncPos ? B_POSITIVE_VSYNC : 0),
	};
	return haikuModeTimings;
}

static inline NvModeTimings ToNvKmsModeTimings(const display_timing &haikuModeTimings) {
	NvModeTimings nvKmsModeTimings {
		.RRx1k        = CalcRefreshRate(haikuModeTimings),
		.pixelClockHz = haikuModeTimings.pixel_clock * 1000,
		.hVisible     = haikuModeTimings.h_display,
		.hSyncStart   = haikuModeTimings.h_sync_start,
		.hSyncEnd     = haikuModeTimings.h_sync_end,
		.hTotal       = haikuModeTimings.h_total,
		.vVisible     = haikuModeTimings.v_display,
		.vSyncStart   = haikuModeTimings.v_sync_start,
		.vSyncEnd     = haikuModeTimings.v_sync_end,
		.vTotal       = haikuModeTimings.v_total,
		.interlaced   = (B_TIMING_INTERLACED & haikuModeTimings.flags) != 0,
		.hSyncPos     = (B_POSITIVE_HSYNC & haikuModeTimings.flags) != 0,
		.hSyncNeg     = (B_POSITIVE_HSYNC & haikuModeTimings.flags) == 0,
		.vSyncPos     = (B_POSITIVE_VSYNC & haikuModeTimings.flags) != 0,
		.vSyncNeg     = (B_POSITIVE_VSYNC & haikuModeTimings.flags) == 0,
	};
	return nvKmsModeTimings;
}

static inline display_mode ToHaikuMode(const NvKmsMode &nvKmsMode) {
	display_mode haikuMode {
		.timing = ToHaikuModeTimings(nvKmsMode.timings),
		.space = B_RGB32,
		.virtual_width  = nvKmsMode.timings.hVisible,
		.virtual_height = nvKmsMode.timings.vVisible,
	};
	return haikuMode;
}

static inline NvKmsMode ToNvKmsMode(const display_mode &haikuMode) {
	NvKmsMode nvKmsMode {
		.timings = ToNvKmsModeTimings(haikuMode.timing),
	};
	return nvKmsMode;
}

// Composition of the cursor image (premultiplied A8R8G8B8), for every head.
static inline NvKmsCompositionParams CursorCompParams()
{
	// app_server's cursor bitmaps are B_RGBA32 that its software cursor
	// blends as premultiplied (HWInterface::_DrawCursor), so blend the same
	// way here and the two look identical.
	NvKmsCompositionParams params {};
	params.colorKeySelect = NVKMS_COMPOSITION_COLOR_KEY_SELECT_DISABLE;
	params.blendingMode[0] = NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA;
	params.blendingMode[1] = NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA;
	return params;
}
