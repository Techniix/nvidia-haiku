#pragma once

#include <PCI.h>

#include <util/Vector.h>

#include "Drivers_cpp.h"
#include "DeviceNamesArray.h"

#include "IntrSafePool.h"
#include "WorkQueue.h"

struct syncobj_module_info;


class NvHaikuControlDevice;
class NvHaikuDevice;


class NvHaikuDriver {
private:
	pci_module_info *fPCI {};
	syncobj_module_info *fSyncobj {};
	class IntrSafePool fIntrSafePool;
	NvHaikuWorkQueue fWorkQueue;

	bool fInitRmDone = false;

	NvHaikuControlDevice *fControlDevice {};
	Vector<NvHaikuDevice*> fDevices;
	DeviceNamesArray fDeviceNamesArray;

	static NvHaikuDriver sInstance;

public:
	~NvHaikuDriver();
	status_t Init();

	static NvHaikuDriver &Instance() {return sInstance;}
	pci_module_info &PCI() {return *fPCI;}
	// Renga's generic/syncobj/v1 module, or nullptr if it is not
	// installed (images before Renga 1424ffb4).
	syncobj_module_info *Syncobj() {return fSyncobj;}
	class IntrSafePool &IntrSafePool() {return fIntrSafePool;}
	NvHaikuWorkQueue &WorkQueue() {return fWorkQueue;}

	uint32 DeviceCount() {return fDevices.Count();}
	NvHaikuDevice *DeviceAt(uint32 index) {return fDevices[index];}

	static status_t InitDriver();
	static void UninitDriver();
	static const char **PublishDevices();
	static DevfsNodeInstance FindDevice(const char *name);
};
