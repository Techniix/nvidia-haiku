#pragma once

extern "C" {
#include <nvkms.h>
}

#include <lock.h>
#include <util/DoublyLinkedList.h>

#include "Drivers_cpp.h"


struct select_sync_pool;


class NvHaikuKmsDevice {
private:
	friend class NvHaikuKmsDeviceHandle;

public:
	status_t Open(uint32 flags, DevfsNodeHandle &handle);
};


class NvHaikuKmsDeviceHandle: public DoublyLinkedListLinkImpl<NvHaikuKmsDeviceHandle> {
private:
	friend class NvHaikuKmsDevice;

	NvHaikuKmsDevice *fDevice {};
	void *fCookie {};

	mutex fSelectPoolMutex = MUTEX_INITIALIZER("NvHaikuSelectPool");
	select_sync_pool* fSelectPool {};
	bool fEventsAvailable = false;

public:
	~NvHaikuKmsDeviceHandle();
	NvHaikuKmsDeviceHandle(NvHaikuKmsDevice *device);

	static NvHaikuKmsDeviceHandle *FromNvKmsHandle(nvkms_per_open_handle_t *pOpenKernel) {return (NvHaikuKmsDeviceHandle*)(pOpenKernel);}

	void EventQueueChanged(NvBool eventsAvailable);

	// The NVKMS per-open data of a modeset device opened by the current team
	// as fd, or NULL when fd is not one. Backs nvkms_get_per_open_data().
	static void *PerOpenDataFromFd(int fd);

	status_t Close();
	status_t Control(uint32 op, void *data, size_t len);
	status_t Select(uint8 event, uint32 ref, selectsync *sync);
	status_t Deselect(uint8 event, selectsync *sync);
};


extern const device_hooks gNvHaikuKmsDeviceClass;
