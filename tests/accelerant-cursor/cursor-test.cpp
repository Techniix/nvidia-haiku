/*
 * Host test for the accelerant's hardware cursor (accelerant/Accelerant.cpp)
 * and the nvrm_cpp_sdk objects under it.
 *
 * It links the real accelerant and SDK sources and replaces only the kernel
 * boundary: open(), close(), ioctl() and the Haiku area calls are wrapped
 * (ld --wrap) by a fake RM + NVKMS that keeps account of every object,
 * mapping, area, surface and device fd. The test drives the accelerant
 * through get_accelerant_hook() the way app_server does, checks after every
 * step that what NVKMS would show matches what app_server asked for, injects
 * failures at each step, and at the end requires that nothing is left
 * allocated.
 *
 * The fake applies the rules the display engine does (NVKMS
 * nvEvoGetHeadSetControlCursorValueC3 and friends): a cursor surface must be
 * A8R8G8B8, pitch layout, square 32/64/128/256, and because the cursor has
 * no pitch register its rows must be packed. A modeset clears the cursor
 * unless the request carries it (nvClearFlipEvoHwState), and unregistering
 * a surface that is on screen takes it off screen.
 */

#include <Accelerant.h>
#include <OS.h>

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <map>
#include <set>
#include <string>
#include <vector>

extern "C" {
#include "nvos.h"
#include "nv_escape.h"
#include "nv-unix-nvos-params-wrappers.h"
#include "nv-ioctl.h"
#include "class/cl0000.h"
#include "class/cl0040.h"
#include "ctrl/ctrl0000/ctrl0000unix.h"
#include "nvkms-ioctl.h"
#include "nvkms-api.h"
#include "nv_dpy_id.h"
}
#include "nv-haiku.h"
#include "nvkms-haiku.h"


static int sFailures = 0;
#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		sFailures++; \
		fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		fprintf(stderr, __VA_ARGS__); \
		fprintf(stderr, "\n"); \
	} \
} while (0)


// #pragma mark - fake kernel

enum FdKind { kFdControl, kFdGpu, kFdModeset };

struct RmObject {
	NvHandle parent;
	NvU32 hClass;
	std::vector<uint8> backing; // for memory objects
};

struct RmMapping {
	NvHandle hMemory;
	NvU64 offset;
	NvU64 length;
	int memFd;
	bool areaCreated;
};

struct Area {
	NvHandle hMemory;
};

struct Surface {
	NvHandle hMemory;
	NvU32 width, height, pitch;
	NvKmsSurfaceMemoryLayout layout;
	NvKmsSurfaceMemoryFormat format;
};

struct FakeKernel {
	std::map<int, FdKind> fds;
	std::map<int, NvHandle> exportedFds;
	NvHandle nextHandle = 0x1000;
	std::map<NvHandle, RmObject> objects;
	NvU64 nextLinear = 0x10000;
	std::map<NvU64, RmMapping> mappings; // by fake pLinearAddress
	area_id nextArea = 100;
	std::map<area_id, Area> areas;
	int staleAreaDeletes = 0;
	NvKmsSurfaceHandle nextSurface = 1;
	std::map<NvKmsSurfaceHandle, Surface> surfaces;

	// display state
	NvKmsSurfaceHandle scanout = 0;
	NvKmsSurfaceHandle cursor = 0;
	NvS16 cursorX = 0, cursorY = 0;
	int scanoutClearedByUnregister = 0;
	// A cursor image update has been pushed to the core channel in the
	// current hook call. Unregistering any surface now makes NVKMS wait for
	// the core channel to go idle, i.e. for that update to reach the screen
	// (nvEvoClearSurfaceUsage -> nvRMSyncEvoChannel), with app_server's lock
	// held. Each set_cursor_bitmap starts by registering its new surface,
	// which marks the start of a new call.
	bool coreUpdatePending = false;
	int stallingUnregisters = 0;
	int modesets = 0;
	int modesetsWithCursor = 0;

	// failure injection: fail the next call of this kind
	std::set<NvU32> failKmsOnce;
	std::set<NvU32> failKmsAlways;
	bool failRmFreeAlways = false;
	int setCursorImageCalls = 0;
	bool failSetModeWithCursor = false;
	bool failRmAllocMemoryOnce = false;
	bool failHaikuMapOnce = false;

	int Handle() { return ++nextHandle; }

	void FreeRecursive(NvHandle h)
	{
		std::vector<NvHandle> children;
		for (auto &[child, obj] : objects)
			if (obj.parent == h)
				children.push_back(child);
		for (NvHandle c : children)
			FreeRecursive(c);
		objects.erase(h);
	}

	bool CursorSurfaceValid(NvKmsSurfaceHandle h, std::string &why)
	{
		auto it = surfaces.find(h);
		if (it == surfaces.end()) { why = "unknown surface"; return false; }
		const Surface &s = it->second;
		if (s.format != NvKmsSurfaceMemoryFormatA8R8G8B8) { why = "format"; return false; }
		if (s.layout != NvKmsSurfaceMemoryLayoutPitch) { why = "layout"; return false; }
		if (s.width != s.height) { why = "not square"; return false; }
		if (s.width != 32 && s.width != 64 && s.width != 128 && s.width != 256) { why = "size"; return false; }
		if (s.pitch != 4 * s.width) { why = "rows not packed (the cursor has no pitch)"; return false; }
		return true;
	}

	static bool CompParamsValid(const NvKmsCompositionParams &p)
	{
		if (p.colorKeySelect != NVKMS_COMPOSITION_COLOR_KEY_SELECT_DISABLE)
			return false;
		switch (p.blendingMode[1]) {
			case NVKMS_COMPOSITION_BLENDING_MODE_OPAQUE:
				return true;
			case NVKMS_COMPOSITION_BLENDING_MODE_NON_PREMULT_ALPHA:
			case NVKMS_COMPOSITION_BLENDING_MODE_PREMULT_ALPHA:
				return p.surfaceAlpha == 0;
			default:
				return false;
		}
	}

	const uint8 *SurfaceBits(NvKmsSurfaceHandle h)
	{
		auto it = surfaces.find(h);
		if (it == surfaces.end()) return nullptr;
		auto obj = objects.find(it->second.hMemory);
		if (obj == objects.end()) return nullptr;
		return obj->second.backing.data();
	}

	int RmIoctl(int fd, NvU32 cmd, void *arg);
	int KmsIoctl(NvU32 cmd, void *arg);
} sKernel;


int FakeKernel::RmIoctl(int fd, NvU32 cmd, void *arg)
{
	switch (cmd) {
		case NV_ESC_RM_ALLOC: {
			auto p = (NVOS21_PARAMETERS*)arg;
			if (p->hClass != NV01_ROOT_CLIENT && objects.count(p->hObjectParent) == 0) {
				p->status = NV_ERR_INVALID_OBJECT_PARENT;
				return 0;
			}
			RmObject obj {p->hObjectParent, p->hClass, {}};
			if (p->hClass == NV01_MEMORY_LOCAL_USER) {
				if (failRmAllocMemoryOnce) {
					failRmAllocMemoryOnce = false;
					p->status = NV_ERR_NO_MEMORY;
					return 0;
				}
				auto mp = (NV_MEMORY_ALLOCATION_PARAMS*)p->pAllocParms;
				obj.backing.assign(mp->size, 0xcd);
			}
			p->hObjectNew = Handle();
			objects[p->hObjectNew] = std::move(obj);
			p->status = NV_OK;
			return 0;
		}
		case NV_ESC_RM_FREE: {
			auto p = (NVOS00_PARAMETERS*)arg;
			if (failRmFreeAlways) {
				p->status = NV_ERR_GENERIC;
				return 0;
			}
			if (objects.count(p->hObjectOld) == 0) {
				p->status = NV_ERR_INVALID_OBJECT_HANDLE;
				return 0;
			}
			for (auto &[lin, m] : mappings)
				CHECK(m.hMemory != p->hObjectOld, "freeing memory 0x%x that is still mapped", p->hObjectOld);
			for (auto &[h, s] : surfaces)
				CHECK(s.hMemory != p->hObjectOld, "freeing memory 0x%x behind registered surface %u", p->hObjectOld, h);
			FreeRecursive(p->hObjectOld);
			p->status = NV_OK;
			return 0;
		}
		case NV_ESC_RM_CONTROL: {
			auto p = (NVOS54_PARAMETERS*)arg;
			if (p->cmd == NV0000_CTRL_CMD_OS_UNIX_EXPORT_OBJECT_TO_FD) {
				auto e = (NV0000_CTRL_OS_UNIX_EXPORT_OBJECT_TO_FD_PARAMS*)p->params;
				if (fds.count(e->fd) == 0 || objects.count(e->object.data.rmObject.hObject) == 0) {
					p->status = NV_ERR_INVALID_ARGUMENT;
					return 0;
				}
				exportedFds[e->fd] = e->object.data.rmObject.hObject;
			}
			p->status = NV_OK;
			return 0;
		}
		case NV_ESC_RM_MAP_MEMORY: {
			auto p = (nv_ioctl_nvos33_parameters_with_fd*)arg;
			if (objects.count(p->params.hMemory) == 0) {
				p->params.status = NV_ERR_INVALID_OBJECT_HANDLE;
				return 0;
			}
			NvU64 lin = nextLinear;
			nextLinear += 0x1000;
			mappings[lin] = {p->params.hMemory, p->params.offset, p->params.length, p->fd, false};
			p->params.pLinearAddress = (void*)lin;
			p->params.status = NV_OK;
			return 0;
		}
		case NV_ESC_RM_UNMAP_MEMORY: {
			auto p = (NVOS34_PARAMETERS*)arg;
			auto it = mappings.find((NvU64)p->pLinearAddress);
			if (it == mappings.end() || it->second.hMemory != p->hMemory) {
				p->status = NV_ERR_INVALID_ARGUMENT;
				return 0;
			}
			for (auto &[id, a] : areas)
				CHECK(a.hMemory != p->hMemory, "unmapping memory 0x%x while area %d still maps it", p->hMemory, (int)id);
			mappings.erase(it);
			p->status = NV_OK;
			return 0;
		}
		case NV_HAIKU_MAP: {
			auto p = (nv_haiku_map_params*)arg;
			if (failHaikuMapOnce) {
				failHaikuMapOnce = false;
				errno = ENOMEM;
				return -1;
			}
			for (auto &[lin, m] : mappings) {
				if (m.memFd == fd && !m.areaCreated) {
					m.areaCreated = true;
					area_id id = nextArea++;
					areas[id] = {m.hMemory};
					p->address = objects[m.hMemory].backing.data() + m.offset;
					return id;
				}
			}
			errno = EINVAL;
			return -1;
		}
	}
	fprintf(stderr, "fake RM: unhandled escape 0x%x\n", cmd);
	errno = ENOTTY;
	return -1;
}


int FakeKernel::KmsIoctl(NvU32 cmd, void *arg)
{
	if (cmd == NVKMS_IOCTL_SET_CURSOR_IMAGE)
		setCursorImageCalls++;
	if (failKmsOnce.erase(cmd) || failKmsAlways.count(cmd)) {
		errno = EINVAL;
		return -1;
	}
	switch (cmd) {
		case NVKMS_IOCTL_ALLOC_DEVICE: {
			auto p = (NvKmsAllocDeviceParams*)arg;
			p->reply.status = NVKMS_ALLOC_DEVICE_STATUS_SUCCESS;
			p->reply.deviceHandle = 1;
			p->reply.numDisps = 1;
			p->reply.dispHandles[0] = 1;
			p->reply.maxCursorSize = 256;
			return 0;
		}
		case NVKMS_IOCTL_FREE_DEVICE:
		case NVKMS_IOCTL_GRAB_OWNERSHIP:
		case NVKMS_IOCTL_SET_DPY_ATTRIBUTE:
			return 0;
		case NVKMS_IOCTL_QUERY_DISP: {
			auto p = (NvKmsQueryDispParams*)arg;
			p->reply.validDpys = nvNvU32ToDpyIdList(1);
			return 0;
		}
		case NVKMS_IOCTL_QUERY_DPY_DYNAMIC_DATA: {
			auto p = (NvKmsQueryDpyDynamicDataParams*)arg;
			p->reply.connected = true;
			return 0;
		}
		case NVKMS_IOCTL_VALIDATE_MODE_INDEX: {
			auto p = (NvKmsValidateModeIndexParams*)arg;
			if (p->request.modeIndex > 0) {
				p->reply.end = true;
				return 0;
			}
			NvModeTimings &t = p->reply.mode.timings;
			t.RRx1k = 60000;
			t.pixelClockHz = 148500000;
			t.hVisible = 1920; t.hSyncStart = 2008; t.hSyncEnd = 2052; t.hTotal = 2200;
			t.vVisible = 1080; t.vSyncStart = 1084; t.vSyncEnd = 1089; t.vTotal = 1125;
			t.hSyncPos = true; t.vSyncPos = true;
			p->reply.valid = true;
			return 0;
		}
		case NVKMS_IOCTL_VALIDATE_MODE: {
			auto p = (NvKmsValidateModeParams*)arg;
			p->reply.valid = true;
			return 0;
		}
		case NVKMS_IOCTL_REGISTER_SURFACE: {
			auto p = (NvKmsRegisterSurfaceParams*)arg;
			auto ex = exportedFds.find(p->request.planes[0].u.fd);
			if (!p->request.useFd || ex == exportedFds.end() || objects.count(ex->second) == 0) {
				errno = EINVAL;
				return -1;
			}
			Surface s {ex->second, p->request.widthInPixels, p->request.heightInPixels,
				(NvU32)p->request.planes[0].pitch, p->request.layout, p->request.format};
			if (s.pitch < 4 * s.width
				|| (NvU64)s.pitch * s.height > objects[ex->second].backing.size()) {
				errno = EINVAL;
				return -1;
			}
			coreUpdatePending = false;
			p->reply.surfaceHandle = nextSurface++;
			surfaces[p->reply.surfaceHandle] = s;
			return 0;
		}
		case NVKMS_IOCTL_UNREGISTER_SURFACE: {
			auto p = (NvKmsUnregisterSurfaceParams*)arg;
			if (surfaces.erase(p->request.surfaceHandle) == 0) {
				errno = EINVAL;
				return -1;
			}
			// NVKMS takes a surface off screen before unregistering it
			// (ClearSurfaceUsage).
			if (cursor == p->request.surfaceHandle)
				cursor = 0;
			if (coreUpdatePending)
				stallingUnregisters++;
			if (scanout == p->request.surfaceHandle) {
				scanout = 0;
				scanoutClearedByUnregister++;
			}
			return 0;
		}
		case NVKMS_IOCTL_SET_MODE: {
			auto p = (NvKmsSetModeParams*)arg;
			auto &head = p->request.disp[0].head[0];
			NvKmsSurfaceHandle fb = head.flip.layer[NVKMS_MAIN_LAYER].surface.handle[0];
			if (surfaces.count(fb) == 0) {
				errno = EINVAL;
				return -1;
			}
			NvKmsSurfaceHandle newCursor = 0;
			if (head.flip.cursor.imageSpecified) {
				if (failSetModeWithCursor) {
					errno = EINVAL;
					return -1;
				}
				newCursor = head.flip.cursor.image.surfaceHandle[NVKMS_LEFT];
				std::string why;
				if (newCursor != 0 && (!CursorSurfaceValid(newCursor, why)
						|| !CompParamsValid(head.flip.cursor.image.cursorCompParams))) {
					errno = EINVAL;
					return -1;
				}
			}
			modesets++;
			if (head.flip.cursor.imageSpecified)
				modesetsWithCursor++;
			scanout = fb;
			// nvClearFlipEvoHwState: what the request doesn't carry is gone
			cursor = newCursor;
			cursorX = head.flip.cursor.positionSpecified ? head.flip.cursor.position.x : 0;
			cursorY = head.flip.cursor.positionSpecified ? head.flip.cursor.position.y : 0;
			return 0;
		}
		case NVKMS_IOCTL_SET_CURSOR_IMAGE: {
			auto p = (NvKmsSetCursorImageParams*)arg;
			NvKmsSurfaceHandle h = p->request.common.surfaceHandle[NVKMS_LEFT];
			std::string why;
			if (h != 0 && !CursorSurfaceValid(h, why)) {
				CHECK(false, "SET_CURSOR_IMAGE with an invalid cursor surface: %s", why.c_str());
				errno = EINVAL;
				return -1;
			}
			coreUpdatePending = true;
			cursor = h;
			return 0;
		}
		case NVKMS_IOCTL_MOVE_CURSOR: {
			auto p = (NvKmsMoveCursorParams*)arg;
			cursorX = p->request.common.x;
			cursorY = p->request.common.y;
			return 0;
		}
	}
	fprintf(stderr, "fake NVKMS: unhandled ioctl %u\n", cmd);
	errno = ENOTTY;
	return -1;
}


extern "C" {

int __real_open(const char *path, int flags, ...);
int __real_close(int fd);

int __wrap_open(const char *path, int flags, ...)
{
	FdKind kind;
	if (strcmp(path, "/dev/" NVIDIA_CONTROL_DEVICE_NAME) == 0)
		kind = kFdControl;
	else if (strncmp(path, "/dev/" NVIDIA_DEVICE_NAME, strlen("/dev/" NVIDIA_DEVICE_NAME)) == 0)
		kind = kFdGpu;
	else if (strcmp(path, "/dev/" NVIDIA_MODESET_DEVICE_NAME) == 0)
		kind = kFdModeset;
	else {
		va_list ap;
		va_start(ap, flags);
		int mode = va_arg(ap, int);
		va_end(ap);
		return __real_open(path, flags, mode);
	}
	int fd = __real_open("/dev/null", O_RDWR | O_CLOEXEC);
	if (fd >= 0)
		sKernel.fds[fd] = kind;
	return fd;
}

int __wrap_close(int fd)
{
	sKernel.fds.erase(fd);
	sKernel.exportedFds.erase(fd);
	return __real_close(fd);
}

int __wrap_ioctl(int fd, unsigned long op, ...)
{
	va_list ap;
	va_start(ap, op);
	void *arg = va_arg(ap, void*);
	va_end(ap);

	auto it = sKernel.fds.find(fd);
	if (it == sKernel.fds.end()) {
		errno = EBADF;
		return -1;
	}
	NvU32 cmd = (NvU32)(op - NV_HAIKU_BASE);
	if (it->second == kFdModeset)
		return sKernel.KmsIoctl(cmd, arg);
	return sKernel.RmIoctl(fd, cmd, arg);
}

status_t __wrap_delete_area(area_id id)
{
	if (sKernel.areas.erase(id) == 0) {
		sKernel.staleAreaDeletes++;
		return B_BAD_VALUE;
	}
	return B_OK;
}

// Driver settings: the test sets what the "file" says.
static const char *sHardwareCursorSetting = nullptr;
static int sSettingsHandles = 0;

void *__wrap_load_driver_settings(const char *name)
{
	if (strcmp(name, "nvidia_gsp") != 0 || sHardwareCursorSetting == nullptr)
		return nullptr;
	sSettingsHandles++;
	return (void*)sHardwareCursorSetting;
}

bool __wrap_get_driver_boolean_parameter(void *handle, const char *key, bool unknownValue, bool noArgValue)
{
	if (handle == nullptr)
		return unknownValue;
	const char *v = (const char*)handle;
	if (strcmp(key, "cursor_timing") == 0)
		return strcmp(v, "timing") == 0 ? true : unknownValue;
	if (strcmp(v, "false") == 0) return false;
	if (strcmp(v, "true") == 0) return true;
	return unknownValue;
}

status_t __wrap_unload_driver_settings(void *handle)
{
	if (handle == nullptr)
		return B_BAD_VALUE;
	sSettingsHandles--;
	return B_OK;
}

#ifdef CURSOR_TEST_HOST
// Not Haiku: libroot's debug_printf() and snooze() are missing.
int debug_printf(const char *format, ...)
{
	if (getenv("CURSOR_TEST_VERBOSE") == nullptr)
		return 0;
	va_list ap;
	va_start(ap, format);
	int res = vfprintf(stderr, format, ap);
	va_end(ap);
	return res;
}

status_t snooze(bigtime_t)
{
	return B_OK;
}

bigtime_t system_time(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (bigtime_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}
#endif

// accelerant/common/decode_edid.c needs Haiku's dprintf; EDID isn't used here.
void edid_decode(void *, const void *)
{
}

}	// extern "C"


// #pragma mark - test


extern "C" void *get_accelerant_hook(uint32 feature, void *data);

struct Hooks {
	init_accelerant init;
	uninit_accelerant uninit;
	accelerant_mode_count modeCount;
	get_mode_list modeList;
	set_display_mode setMode;
	move_cursor move;
	show_cursor show;
	set_cursor_bitmap setBitmap;

	void Load()
	{
		init = (init_accelerant)get_accelerant_hook(B_INIT_ACCELERANT, nullptr);
		uninit = (uninit_accelerant)get_accelerant_hook(B_UNINIT_ACCELERANT, nullptr);
		modeCount = (accelerant_mode_count)get_accelerant_hook(B_ACCELERANT_MODE_COUNT, nullptr);
		modeList = (get_mode_list)get_accelerant_hook(B_GET_MODE_LIST, nullptr);
		setMode = (set_display_mode)get_accelerant_hook(B_SET_DISPLAY_MODE, nullptr);
		move = (move_cursor)get_accelerant_hook(B_MOVE_CURSOR, nullptr);
		show = (show_cursor)get_accelerant_hook(B_SHOW_CURSOR, nullptr);
		setBitmap = (set_cursor_bitmap)get_accelerant_hook(B_SET_CURSOR_BITMAP, nullptr);
	}
};

// What app_server last told the accelerant, and so what should be on screen.
struct Expected {
	bool hardware = false; // the last set_cursor_bitmap succeeded
	bool visible = false;
	int x = 0, y = 0;
	int hotX = 0, hotY = 0;
	int width = 0, height = 0;
	std::vector<uint32> pixels;
} sExpected;

static uint32 sSeed = 12345;
static uint32 Random()
{
	sSeed = sSeed * 1103515245 + 12345;
	return sSeed >> 8;
}

static std::vector<uint32> MakeCursor(int width, int height, int stride)
{
	std::vector<uint32> bits(stride * height);
	for (auto &b : bits)
		b = Random() | 0x01000000;
	return bits;
}

static status_t SetBitmap(Hooks &h, int width, int height, int hotX, int hotY, color_space space = B_RGBA32, int padding = 3)
{
	int stride = width + padding;
	auto bits = MakeCursor(width, height, stride);
	status_t res = h.setBitmap(width, height, hotX, hotY, space, stride * 4, (const uint8*)bits.data());
	if (res == B_OK) {
		sExpected.hardware = true;
		sExpected.width = width;
		sExpected.height = height;
		sExpected.hotX = hotX;
		sExpected.hotY = hotY;
		sExpected.pixels.assign(width * height, 0);
		for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
				sExpected.pixels[y * width + x] = bits[y * stride + x];
	}
	return res;
}

static void CheckScreen(const char *where)
{
	FakeKernel &k = sKernel;
	CHECK(k.scanout != 0 && k.surfaces.count(k.scanout), "%s: nothing scanned out", where);
	if (!(sExpected.hardware && sExpected.visible)) {
		CHECK(k.cursor == 0, "%s: a cursor is shown that should be hidden", where);
		return;
	}
	CHECK(k.cursor != 0, "%s: no cursor shown", where);
	if (k.cursor == 0)
		return;
	std::string why;
	CHECK(k.CursorSurfaceValid(k.cursor, why), "%s: shown cursor invalid: %s", where, why.c_str());
	CHECK(k.cursorX == sExpected.x - sExpected.hotX && k.cursorY == sExpected.y - sExpected.hotY,
		"%s: cursor at %d,%d, want %d,%d", where, k.cursorX, k.cursorY,
		sExpected.x - sExpected.hotX, sExpected.y - sExpected.hotY);
	const Surface &s = k.surfaces[k.cursor];
	const uint32 *bits = (const uint32*)k.SurfaceBits(k.cursor);
	CHECK(bits != nullptr, "%s: no memory behind the cursor", where);
	if (bits == nullptr)
		return;
	int bad = 0;
	for (uint32 y = 0; y < s.height; y++) {
		for (uint32 x = 0; x < s.width; x++) {
			uint32 want = ((int)x < sExpected.width && (int)y < sExpected.height)
				? sExpected.pixels[y * sExpected.width + x] : 0;
			if (bits[y * (s.pitch / 4) + x] != want)
				bad++;
		}
	}
	CHECK(bad == 0, "%s: %d cursor pixels differ", where, bad);
}

// Resources that are not the cursor: the client/device/subdevice objects and
// the framebuffers (current and previous).
struct Counts {
	size_t objects, mappings, areas, surfaces, fds;
};

static Counts Snapshot()
{
	return {sKernel.objects.size(), sKernel.mappings.size(), sKernel.areas.size(),
		sKernel.surfaces.size(), sKernel.fds.size()};
}

static void CheckCounts(const char *where, const Counts &a, const Counts &b)
{
	CHECK(a.objects == b.objects && a.mappings == b.mappings && a.areas == b.areas
		&& a.surfaces == b.surfaces && a.fds == b.fds,
		"%s: objects %zu->%zu mappings %zu->%zu areas %zu->%zu surfaces %zu->%zu fds %zu->%zu",
		where, a.objects, b.objects, a.mappings, b.mappings, a.areas, b.areas,
		a.surfaces, b.surfaces, a.fds, b.fds);
}

static void SetDisplayMode(Hooks &h)
{
	display_mode mode {};
	CHECK(h.modeCount() == 1, "mode count");
	h.modeList(&mode);
	CHECK(h.setMode(&mode) == B_OK, "set_display_mode failed");
}

static void TestHardwareCursor()
{
	FakeKernel &k = sKernel;
	sHardwareCursorSetting = nullptr; // no settings file: enabled

	Hooks h;
	h.Load();
	CHECK(h.move && h.show && h.setBitmap, "cursor hooks not exported");
	CHECK(get_accelerant_hook(B_SET_CURSOR_SHAPE, nullptr) == nullptr,
		"B_SET_CURSOR_SHAPE should stay unexported");
	if (!(h.move && h.show && h.setBitmap))
		return;

	int devFd = open("/dev/null", O_RDWR);
	CHECK(h.init(devFd) == B_OK, "init_accelerant");

	// app_server: set the mode, then the cursor (AccelerantHWInterface::SetCursor)
	SetDisplayMode(h);
	CHECK(k.modesetsWithCursor == 0, "first modeset carried a cursor that isn't shown yet");
	Counts base = Snapshot();

	CHECK(SetBitmap(h, 16, 16, 1, 1) == B_OK, "first cursor");
	sExpected.x = 100; sExpected.y = 200;
	h.move(sExpected.x, sExpected.y);
	sExpected.visible = true;
	h.show(true);
	CheckScreen("first cursor");
	Counts one = Snapshot();
	CHECK(one.surfaces == base.surfaces + 1 && one.areas == base.areas + 1
		&& one.objects == base.objects + 1 && one.mappings == base.mappings + 1,
		"a cursor should cost exactly one memory object, mapping, area and surface");
	// From the second image on, the replaced one is kept until the next
	// change: two of each, and never more.
	CHECK(SetBitmap(h, 16, 16, 1, 1) == B_OK, "second cursor");
	CheckScreen("second cursor");
	Counts withCursor = Snapshot();
	CHECK(withCursor.surfaces == base.surfaces + 2 && withCursor.areas == base.areas + 2
		&& withCursor.objects == base.objects + 2 && withCursor.mappings == base.mappings + 2,
		"steady state should be two cursor images");

	// Shape changes, moves, show/hide, all sizes the hardware takes.
	for (int i = 0; i < 400; i++) {
		int w = 1 + Random() % 256;
		int hgt = 1 + Random() % 256;
		if (i % 7 == 0) { w = 1 + Random() % 32; hgt = 1 + Random() % 32; }
		int hotX = Random() % w, hotY = Random() % hgt;
		CHECK(SetBitmap(h, w, hgt, hotX, hotY, B_RGBA32, Random() % 5) == B_OK, "%dx%d cursor refused", w, hgt);
		CheckScreen("after a shape change");
		sExpected.x = Random() % 1920;
		sExpected.y = Random() % 1080;
		h.move(sExpected.x, sExpected.y);
		CheckScreen("after a move");
		if (i % 5 == 0) {
			sExpected.visible = false;
			h.show(false);
			CheckScreen("hidden");
			// change shape and move while hidden, then show again
			if (i % 10 == 0) {
				CHECK(SetBitmap(h, 24, 24, 12, 12) == B_OK, "cursor while hidden");
				sExpected.x = 5; sExpected.y = 5;
				h.move(sExpected.x, sExpected.y);
			}
			sExpected.visible = true;
			h.show(true);
			CheckScreen("shown again");
		}
		CheckCounts("cursor churn", withCursor, Snapshot());
	}
	h.show(true); // no change
	CheckScreen("show twice");
	CHECK(k.stallingUnregisters == 0, "%d shape changes freed the image they had just replaced"
		" (NVKMS would wait for the display with app_server's lock held)", k.stallingUnregisters);

	// Refusals: app_server falls back to its software cursor. Nothing may be
	// allocated, and what is on screen must not change.
	Counts before = Snapshot();
	CHECK(SetBitmap(h, 257, 10, 0, 0) != B_OK, "257 wide accepted");
	CHECK(SetBitmap(h, 10, 300, 0, 0) != B_OK, "300 high accepted");
	CHECK(SetBitmap(h, 16, 16, 0, 0, B_RGB32) != B_OK, "B_RGB32 accepted");
	CHECK(SetBitmap(h, 16, 16, 0, 0, B_CMAP8) != B_OK, "B_CMAP8 accepted");
	CHECK(SetBitmap(h, 0, 16, 0, 0) != B_OK, "0 wide accepted");
	CHECK(h.setBitmap(16, 16, 0, 0, B_RGBA32, 16, (const uint8*)sExpected.pixels.data()) != B_OK,
		"bytesPerRow below 4*width accepted");
	CHECK(h.setBitmap(16, 16, 0, 0, B_RGBA32, 64, nullptr) != B_OK, "NULL bits accepted");
	CheckScreen("after refusals");
	CheckCounts("refusals", before, Snapshot());

	// Kernel failures at each step of making a cursor.
	struct { const char *what; void (*arm)(); } faults[] = {
		{"RM alloc", [] { sKernel.failRmAllocMemoryOnce = true; }},
		{"register surface", [] { sKernel.failKmsOnce.insert(NVKMS_IOCTL_REGISTER_SURFACE); }},
		{"map (NV_HAIKU_MAP)", [] { sKernel.failHaikuMapOnce = true; }},
		{"set cursor image", [] { sKernel.failKmsOnce.insert(NVKMS_IOCTL_SET_CURSOR_IMAGE); }},
	};
	for (auto &f : faults) {
		before = Snapshot();
		Expected saved = sExpected;
		f.arm();
		status_t res = SetBitmap(h, 40, 40, 3, 3);
		CHECK(res != B_OK, "%s failure not reported", f.what);
		sExpected = saved;
		CheckScreen(f.what);
		// The retired image may already be gone (it is freed before a new
		// one is submitted); nothing else may change.
		Counts after = Snapshot();
		size_t dropped = before.surfaces - after.surfaces;
		CHECK(dropped <= 1, "%s: %zu surfaces freed", f.what, dropped);
		Counts expect = {before.objects - dropped, before.mappings - dropped,
			before.areas - dropped, before.surfaces - dropped, before.fds};
		CheckCounts(f.what, expect, after);
		k.failKmsOnce.clear();
	}
	// After a failed set_cursor_bitmap app_server calls show_cursor(false)
	// and uses the software cursor; the next good one brings it back.
	sExpected.visible = false;
	h.show(false);
	CheckScreen("software fallback");
	CHECK(SetBitmap(h, 32, 32, 0, 0) == B_OK, "cursor after failures");
	sExpected.visible = true;
	h.show(true);
	CheckScreen("back to hardware");

	// A modeset with the cursor shown keeps it, in the same request.
	int withCursorBefore = k.modesetsWithCursor;
	SetDisplayMode(h);
	CHECK(k.modesetsWithCursor == withCursorBefore + 1, "modeset didn't carry the cursor");
	CheckScreen("after a modeset");
	// ... and if the driver refuses that, the mode still gets set and the
	// cursor comes back.
	k.failSetModeWithCursor = true;
	SetDisplayMode(h);
	k.failSetModeWithCursor = false;
	CheckScreen("after a modeset that refused the cursor");
	// ... and a hidden cursor stays hidden.
	sExpected.visible = false;
	h.show(false);
	SetDisplayMode(h);
	CheckScreen("modeset with the cursor hidden");
	sExpected.visible = true;
	h.show(true);
	CheckScreen("shown after a hidden modeset");
	CHECK(k.scanoutClearedByUnregister == 0, "a framebuffer on screen was unregistered");

	// show_cursor and the post-modeset restore can't report errors. If NVKMS
	// refuses the image there, the cursor must come back by itself.
	sExpected.visible = false;
	h.show(false);
	k.failKmsOnce.insert(NVKMS_IOCTL_SET_CURSOR_IMAGE);
	sExpected.visible = true;
	h.show(true);
	h.move(sExpected.x = 300, sExpected.y = 300);
	CheckScreen("show refused, then a move");
	k.failSetModeWithCursor = true;
	k.failKmsOnce.insert(NVKMS_IOCTL_SET_CURSOR_IMAGE);
	SetDisplayMode(h);
	k.failSetModeWithCursor = false;
	h.move(sExpected.x = 310, sExpected.y = 310);
	CheckScreen("restore after modeset refused, then a move");
	// A lasting refusal is retried, but rarely: on the next move, then once
	// per 64.
	k.failKmsAlways.insert(NVKMS_IOCTL_SET_CURSOR_IMAGE);
	sExpected.visible = false;
	h.show(false);
	sExpected.visible = true;
	h.show(true);
	int calls = k.setCursorImageCalls;
	for (int i = 0; i < 650; i++)
		h.move(400, 400);
	int retries = k.setCursorImageCalls - calls;
	CHECK(retries >= 5 && retries <= 12, "%d image retries over 650 moves", retries);
	k.failKmsAlways.clear();
	for (int i = 0; i < 70; i++)
		h.move(sExpected.x = 420, sExpected.y = 420);
	CheckScreen("after a lasting refusal clears");

	// Modesets replace framebuffers; that must not leak either.
	before = Snapshot();
	for (int i = 0; i < 20; i++)
		SetDisplayMode(h);
	CheckCounts("repeated modesets", before, Snapshot());
	CheckScreen("after repeated modesets");

	h.uninit();
	close(devFd);
}

static void TestTimingEnabled()
{
	// Exercise the timing code (and the hooks with it) in a child: the
	// setting is read once per process.
	pid_t pid = fork();
	if (pid == 0) {
		sFailures = 0;
		sHardwareCursorSetting = "timing";
		Hooks h;
		h.Load();
		int devFd = open("/dev/null", O_RDWR);
		CHECK(h.init(devFd) == B_OK, "init_accelerant");
		SetDisplayMode(h);
		for (int i = 0; i < 70; i++)
			CHECK(SetBitmap(h, 20, 20, 0, 0) == B_OK, "cursor with timing");
		h.show(true);
		for (int i = 0; i < 5000; i++)
			h.move(i % 1920, i % 1080);
		h.uninit();
		close(devFd);
		CHECK(sKernel.surfaces.empty() && sKernel.objects.empty() && sKernel.areas.empty(),
			"leak with timing on");
		_exit(sFailures == 0 ? 0 : 1);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "timing test failed");
}

static void TestCleanupFailures()
{
	// Cleanup that fails (unregister, RM free) must not terminate the
	// process: destructors can't throw. Resources leak in the fake here, so
	// run it in a child and only check that it survives.
	pid_t pid = fork();
	if (pid == 0) {
		sFailures = 0;
		Hooks h;
		h.Load();
		int devFd = open("/dev/null", O_RDWR);
		CHECK(h.init(devFd) == B_OK, "init_accelerant");
		SetDisplayMode(h);
		CHECK(SetBitmap(h, 20, 20, 0, 0) == B_OK, "cursor");
		h.show(true);
		sKernel.failKmsAlways.insert(NVKMS_IOCTL_UNREGISTER_SURFACE);
		sKernel.failRmFreeAlways = true;
		for (int i = 0; i < 5; i++)
			SetBitmap(h, 30, 30, 0, 0);
		sKernel.failKmsAlways.insert(NVKMS_IOCTL_SET_CURSOR_IMAGE);
		SetBitmap(h, 40, 40, 0, 0); // fails, and so does freeing what it made
		SetDisplayMode(h);
		h.uninit();
		close(devFd);
		_exit(sFailures == 0 ? 0 : 1);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0,
		"failing cleanup killed the process (status %#x)", status);
}

static void TestDisabledBySettings()
{
	// The hooks read the setting once per process; run this in a child.
	pid_t pid = fork();
	if (pid == 0) {
		sFailures = 0;
		sHardwareCursorSetting = "false";
		bool none = get_accelerant_hook(B_MOVE_CURSOR, nullptr) == nullptr
			&& get_accelerant_hook(B_SHOW_CURSOR, nullptr) == nullptr
			&& get_accelerant_hook(B_SET_CURSOR_BITMAP, nullptr) == nullptr;
		CHECK(none, "hardware_cursor false still exports cursor hooks");
		CHECK(sSettingsHandles == 0, "settings handle leaked");
		_exit(sFailures == 0 ? 0 : 1);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0, "settings opt-out test failed");
}

int main()
{
	TestDisabledBySettings();
	TestTimingEnabled();
	TestCleanupFailures();
	TestHardwareCursor();

	FakeKernel &k = sKernel;
	CHECK(k.objects.empty(), "%zu RM objects left after uninit", k.objects.size());
	CHECK(k.mappings.empty(), "%zu RM mappings left after uninit", k.mappings.size());
	CHECK(k.areas.empty(), "%zu areas left after uninit", k.areas.size());
	CHECK(k.surfaces.empty(), "%zu NVKMS surfaces left after uninit", k.surfaces.size());
	CHECK(k.fds.empty(), "%zu device fds left open after uninit", k.fds.size());
	CHECK(k.staleAreaDeletes == 0, "%d delete_area() calls on areas that were already gone", k.staleAreaDeletes);
	CHECK(sSettingsHandles == 0, "settings handle leaked");

	if (sFailures != 0) {
		fprintf(stderr, "%d check(s) failed\n", sFailures);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}
