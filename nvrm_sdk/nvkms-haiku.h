#pragma once

#include <drivers/Drivers.h>
#include <OS.h>


#define NVIDIA_MODESET_DEVICE_NAME "nvidia-modeset"


enum {
	NV_HAIKU_KMS_BASE = B_DEVICE_OP_CODES_END + 1,

	// Haiku's own ops, far above the NVKMS ioctl numbers (below 0x100),
	// which are passed on to nvKmsIoctl() as base + NVKMS_IOCTL_*.

	// Kernel-only: return the NvHaikuKmsDeviceHandle behind a descriptor
	// (KmsDevice.cpp).
	NV_HAIKU_KMS_GET_HANDLE = NV_HAIKU_KMS_BASE + 0x10000,

	// On a descriptor used as an NVKMS "unicast event" (NOTIFY_VBLANK):
	// how many notifications arrived since the last call, and the time of
	// the newest, then mark the descriptor not readable until the next one.
	// Buffer: nv_haiku_kms_unicast_events. A repeating vblank request
	// (NvKmsNotifyVblankRequest::repeat) is read this way once per wakeup.
	NV_HAIKU_KMS_TAKE_UNICAST_EVENTS = NV_HAIKU_KMS_BASE + 0x10001,
};

typedef struct {
	uint32		count;		// notifications since the last call; more than one
							// means vblanks went by unread
	uint32		reserved;
	bigtime_t	time;		// system_time() when the newest one arrived
} nv_haiku_kms_unicast_events;
