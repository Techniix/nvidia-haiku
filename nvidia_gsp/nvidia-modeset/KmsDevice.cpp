#include "KmsDevice.h"

#include <new>

#include <team.h>

#include <AutoDeleter.h>
#include <util/AutoLock.h>

#include <fs/fd.h>
#include <fs/select_sync_pool.h>

#include "KmsDriver.h"


#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


enum {
	NV_HAIKU_KMS_BASE = B_DEVICE_OP_CODES_END + 1,

	// Kernel-only: return the NvHaikuKmsDeviceHandle behind a descriptor.
	// Far above the NVKMS ioctl numbers (below 0x100), which are passed on
	// to nvKmsIoctl() as base + NVKMS_IOCTL_*.
	NV_HAIKU_KMS_GET_HANDLE = NV_HAIKU_KMS_BASE + 0x10000,
};


// Every open modeset handle. PerOpenDataFromFd() accepts a handle only if
// it is in this list: the descriptor it was given may belong to any driver,
// and a driver that answers an unknown op with B_OK could otherwise make us
// trust an arbitrary pointer. The RM device's equivalent
// (NvHaikuBaseDeviceHandle::FromFd) has a TODO for this check.
static mutex sHandleListLock = MUTEX_INITIALIZER("nvidia-modeset handles");
static DoublyLinkedList<NvHaikuKmsDeviceHandle> sHandleList;


const device_hooks gNvHaikuKmsDeviceClass = GetDevfsNodeClass<NvHaikuKmsDriver, NvHaikuKmsDevice, NvHaikuKmsDeviceHandle>();


status_t NvHaikuKmsDevice::Open(uint32 flags, DevfsNodeHandle &handle)
{
	ObjectDeleter<NvHaikuKmsDeviceHandle> handleImpl(new (std::nothrow) NvHaikuKmsDeviceHandle(this));
	if (!handleImpl.IsSet()) {
		return B_NO_MEMORY;
	}

	{
		MutexLocker lock(&NvHaikuKmsDriver::Instance().Locker());
		handleImpl->fCookie = nvKmsOpen(team_get_current_team_id(), NVKMS_CLIENT_USER_SPACE, (nvkms_per_open_handle_t*)handleImpl.Get());
	}
	if (handleImpl->fCookie == NULL) {
		return B_NO_MEMORY;
	}

	{
		MutexLocker listLock(&sHandleListLock);
		sHandleList.Add(handleImpl.Get());
	}

	handle = DevfsNodeHandle(nullptr, handleImpl.Detach());

	return B_OK;
}


NvHaikuKmsDeviceHandle::~NvHaikuKmsDeviceHandle()
{
	if (fCookie != NULL) {
		{
			MutexLocker listLock(&sHandleListLock);
			sHandleList.Remove(this);
		}
		MutexLocker lock(&NvHaikuKmsDriver::Instance().Locker());
		nvKmsClose(fCookie);
	}
}

NvHaikuKmsDeviceHandle::NvHaikuKmsDeviceHandle(NvHaikuKmsDevice *device):
	fDevice(device)
{
}

void NvHaikuKmsDeviceHandle::EventQueueChanged(NvBool eventsAvailable)
{
	MutexLocker _(&fSelectPoolMutex);
	fEventsAvailable = eventsAvailable;
	if (fEventsAvailable) {
		notify_select_event_pool(fSelectPool, B_SELECT_READ);
	}
}

status_t NvHaikuKmsDeviceHandle::Close()
{
	return B_OK;
}

status_t NvHaikuKmsDeviceHandle::Control(uint32 op, void *data, size_t len)
{
	if (op < NV_HAIKU_KMS_BASE) {
		return B_DEV_INVALID_IOCTL;
	}

	if (op == NV_HAIKU_KMS_GET_HANDLE) {
		// Answered without the driver lock: the caller is
		// nvkms_get_per_open_data(), which NVKMS calls from inside
		// nvKmsIoctl() with that lock held. Taking it here would deadlock.
		if (!IS_KERNEL_ADDRESS(data) || len != sizeof(void*))
			return B_DEV_INVALID_IOCTL;
		*(NvHaikuKmsDeviceHandle**)data = this;
		return B_OK;
	}

	uint32 cmd = op - NV_HAIKU_KMS_BASE;

	NvBool res;
	{
		MutexLocker lock(&NvHaikuKmsDriver::Instance().Locker());
		res = nvKmsIoctl(fCookie, cmd, (addr_t)data, len);
	}
	return res ? B_OK : EINVAL;
}

status_t NvHaikuKmsDeviceHandle::Select(uint8 event, uint32 ref, selectsync *sync)
{
	MutexLocker _(&fSelectPoolMutex);
	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	CHECK_RET(add_select_sync_pool_entry(&fSelectPool, sync, event));

	if (fEventsAvailable) {
		notify_select_event(sync, event);
	}

	return B_OK;
}

status_t NvHaikuKmsDeviceHandle::Deselect(uint8 event, selectsync *sync)
{
	MutexLocker _(&fSelectPoolMutex);

	if (event != B_SELECT_READ)
		return B_BAD_VALUE;

	CHECK_RET(remove_select_sync_pool_entry(&fSelectPool, sync, event));

	return B_OK;
}


/*
 * NVKMS uses this for ioctls that name a second modeset descriptor: a
 * "unicast event" descriptor that NVKMS_IOCTL_NOTIFY_VBLANK signals, and the
 * descriptors that GRANT_SURFACE, ACQUIRE_SURFACE and the swap-group and
 * permission ioctls pass surfaces and rights through.
 *
 * Linux's version (nvidia-modeset-linux.c) looks the descriptor up with
 * fget(), checks that it is an nvidia-modeset file and returns the NVKMS
 * data of that open. The descriptor is resolved in the calling team, which
 * is the team making the ioctl, as on Linux.
 *
 * Lifetime, as on Linux: NVKMS calls this with the driver lock held, and a
 * handle frees its NVKMS data (nvKmsClose) only under that same lock, so the
 * returned pointer stays valid for the rest of the ioctl even if another
 * thread closes the descriptor meanwhile.
 */
void *
NvHaikuKmsDeviceHandle::PerOpenDataFromFd(int fd)
{
	NvHaikuKmsDeviceHandle *handle = NULL;
	if (user_fd_kernel_ioctl(fd, NV_HAIKU_KMS_GET_HANDLE, &handle,
			sizeof(handle)) < B_OK || handle == NULL)
		return NULL;

	MutexLocker listLock(&sHandleListLock);
	for (NvHaikuKmsDeviceHandle *it = sHandleList.First(); it != NULL;
			it = sHandleList.GetNext(it)) {
		if (it == handle)
			return handle->fCookie;
	}
	return NULL;
}
