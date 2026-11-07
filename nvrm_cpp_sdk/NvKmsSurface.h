#pragma once

extern "C" {
#include "nvkms-api.h"
}


class NvKmsDevice;

class NvKmsSurface {
private:
	NvKmsDevice *fKmsDev;
	NvKmsSurfaceHandle fHandle;

	void Delete();

public:
	inline NvKmsSurface();
	inline NvKmsSurface(NvKmsDevice &kmsDev, NvKmsSurfaceHandle handle);
	inline NvKmsSurface(NvKmsSurface &&other);
	inline ~NvKmsSurface();

	inline NvKmsSurface &operator=(NvKmsSurface &&other);

	NvKmsSurface(const NvKmsSurface &other) = delete;
	NvKmsSurface &operator=(const NvKmsSurface &other) = delete;

	inline bool IsSet() const {return fHandle != 0;}
	inline NvKmsDevice *Device() const {return fKmsDev;}
	inline NvKmsSurfaceHandle Get() const;
	inline NvKmsSurfaceHandle Detach();
};


NvKmsSurface::NvKmsSurface():
	fKmsDev(nullptr), fHandle(0)
{
}

NvKmsSurface::NvKmsSurface(NvKmsDevice &kmsDev, NvKmsSurfaceHandle handle):
	fKmsDev(&kmsDev), fHandle(handle)
{
}

NvKmsSurface::NvKmsSurface(NvKmsSurface &&other):
	fKmsDev(other.fKmsDev), fHandle(other.fHandle)
{
	other.fHandle = 0;
}

NvKmsSurface::~NvKmsSurface()
{
	// Must not throw: see NvRmObject::FreeQuietly().
	if (fHandle != 0) {
		try {
			Delete();
		} catch (...) {
		}
	}
}

NvKmsSurface &NvKmsSurface::operator=(NvKmsSurface &&other)
{
	if (this == &other) {
		return *this;
	}
	if (fHandle != 0) {
		try {
			Delete();
		} catch (...) {
		}
	}
	fKmsDev = other.fKmsDev;
	fHandle = other.fHandle;
	other.fHandle = 0;
	return *this;
}

NvKmsSurfaceHandle NvKmsSurface::Detach()
{
	NvKmsSurfaceHandle res = fHandle;
	fHandle = 0;
	return res;
}

NvKmsSurfaceHandle NvKmsSurface::Get() const
{
	return fHandle;
}
