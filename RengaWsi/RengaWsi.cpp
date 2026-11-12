/*
 * RengaWsi: the Vulkan surface layer of Quasar on Renga.
 *
 * Swapchains, acquire and present are implemented by NVK, in Mesa's common
 * WSI code (src/vulkan/wsi/wsi_common_renga.cpp on Techniix/mesa, branch
 * renga). This layer only does what a driver cannot:
 *
 *  1. Keep surface objects compatible with existing applications. Zink
 *     (zink_kopper_haiku.cpp), OBS Studio and Eglantine create a surface with
 *     vkCreateHeadlessSurfaceEXT, cast the VkSurfaceKHR to
 *     VKLayerSurfaceBase and call its virtual SetBitmapHook() to attach a
 *     window. A surface made by the Vulkan loader is a plain loader struct,
 *     so the object behind the handle has to come from a layer. This layer
 *     returns its own object, which holds the real (loader) surface.
 *
 *  2. Get the hook to the driver. The loader gives the driver its own copy
 *     of each surface, which a layer cannot name, so the hook travels with
 *     vkCreateSwapchainKHR, chained as wsi_renga_swapchain_bitmap_hook
 *     (sType 0x52474102). The loader passes pNext chains through.
 *
 *  3. Answer surface queries for the window: the current extent is the
 *     window's size (from the hook), and only the formats the Renga
 *     swapchain can show (BGRA8, which is B_RGB32 in memory) are offered.
 *
 * Everything else goes straight down the chain. The layer is written new;
 * only the BitmapHook and VKLayerSurfaceBase interfaces of VideoStreamsWsi
 * are matched, because applications are compiled against them.
 *
 * See docs/RENGA-WSI.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <mutex>
#include <new>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define LAYER_EXPORT extern "C" __attribute__((visibility("default")))


// #pragma mark - interfaces applications compile against


class BBitmap;

// As declared by VideoStreamsWsi and Zink: virtual destructor, GetSize,
// SetBitmap, in that order.
class BitmapHook {
public:
	virtual ~BitmapHook() {};
	virtual void GetSize(uint32_t &width, uint32_t &height) = 0;
	virtual BBitmap *SetBitmap(BBitmap *bmp) = 0;
};

// As declared by VideoStreamsWsi and zink_kopper_haiku.cpp: the object a
// VkSurfaceKHR from vkCreateHeadlessSurfaceEXT points to.
class VKLayerSurfaceBase {
public:
	virtual ~VKLayerSurfaceBase() {};
	virtual void SetBitmapHook(BitmapHook *hook) = 0;
};


// Shared with Mesa's wsi_common_renga.h; keep identical.
#define WSI_RENGA_STRUCTURE_TYPE_SWAPCHAIN_BITMAP_HOOK ((VkStructureType)0x52474102)

struct wsi_renga_swapchain_bitmap_hook {
	VkStructureType sType;
	const void *pNext;
	void *bitmap_hook;
};


// #pragma mark - debug output


static bool
Debug()
{
	static const bool enabled = getenv("RENGA_WSI_DEBUG") != NULL;
	return enabled;
}

#define TRACE(...) do { if (Debug()) fprintf(stderr, "RengaWsi: " __VA_ARGS__); } while (0)


// #pragma mark - dispatch


// Loader convention: the first pointer of a dispatchable object is its
// dispatch table, shared by an instance and its physical devices, and by a
// device and its queues and command buffers.
typedef void *DispatchKey;

static inline DispatchKey
KeyOf(const void *object)
{
	return *(DispatchKey *)object;
}


struct InstanceData {
	VkInstance instance;
	PFN_vkGetInstanceProcAddr gipa;

	PFN_vkDestroyInstance DestroyInstance;
	PFN_vkCreateHeadlessSurfaceEXT CreateHeadlessSurfaceEXT;
	PFN_vkDestroySurfaceKHR DestroySurfaceKHR;
	PFN_vkGetPhysicalDeviceSurfaceSupportKHR GetPhysicalDeviceSurfaceSupportKHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR GetPhysicalDeviceSurfaceCapabilitiesKHR;
	PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR GetPhysicalDeviceSurfaceCapabilities2KHR;
	PFN_vkGetPhysicalDeviceSurfaceFormatsKHR GetPhysicalDeviceSurfaceFormatsKHR;
	PFN_vkGetPhysicalDeviceSurfaceFormats2KHR GetPhysicalDeviceSurfaceFormats2KHR;
	PFN_vkGetPhysicalDeviceSurfacePresentModesKHR GetPhysicalDeviceSurfacePresentModesKHR;
	PFN_vkGetPhysicalDevicePresentRectanglesKHR GetPhysicalDevicePresentRectanglesKHR;
};

struct DeviceData {
	VkDevice device;
	PFN_vkGetDeviceProcAddr gdpa;

	PFN_vkDestroyDevice DestroyDevice;
	PFN_vkCreateSwapchainKHR CreateSwapchainKHR;
	PFN_vkCreateSharedSwapchainsKHR CreateSharedSwapchainsKHR;
	PFN_vkGetDeviceGroupSurfacePresentModesKHR GetDeviceGroupSurfacePresentModesKHR;
};

static std::mutex sLock;
static std::unordered_map<DispatchKey, InstanceData *> sInstances;
static std::unordered_map<DispatchKey, DeviceData *> sDevices;


static InstanceData *
InstanceOf(const void *dispatchable)
{
	std::lock_guard<std::mutex> lock(sLock);
	auto it = sInstances.find(KeyOf(dispatchable));
	return it == sInstances.end() ? nullptr : it->second;
}


static DeviceData *
DeviceOf(const void *dispatchable)
{
	std::lock_guard<std::mutex> lock(sLock);
	auto it = sDevices.find(KeyOf(dispatchable));
	return it == sDevices.end() ? nullptr : it->second;
}


// #pragma mark - surfaces


class RengaSurface final: public VKLayerSurfaceBase {
public:
	static constexpr uint32_t kMagic = 0x52575346;	// 'RWSF'

	uint32_t magic = kMagic;
	VkSurfaceKHR real;			// the loader's headless surface
	BitmapHook *hook = nullptr;	// set by the application, may change

	explicit RengaSurface(VkSurfaceKHR realSurface): real(realSurface) {}
	~RengaSurface() override { magic = 0; }

	void SetBitmapHook(BitmapHook *newHook) override
	{
		TRACE("SetBitmapHook(%p) on surface %p\n", (void *)newHook, (void *)this);
		hook = newHook;
	}
};


// Every surface this layer made. A handle that is not in the set is passed
// down unchanged: it may come from another layer or extension.
static std::unordered_map<uint64_t, RengaSurface *> sSurfaces;


static RengaSurface *
SurfaceOf(VkSurfaceKHR handle)
{
	if (handle == VK_NULL_HANDLE)
		return nullptr;
	std::lock_guard<std::mutex> lock(sLock);
	auto it = sSurfaces.find((uint64_t)handle);
	return it == sSurfaces.end() ? nullptr : it->second;
}


static VkSurfaceKHR
RealSurface(VkSurfaceKHR handle)
{
	RengaSurface *surface = SurfaceOf(handle);
	return surface != nullptr ? surface->real : handle;
}


static VkResult VKAPI_CALL
Layer_CreateHeadlessSurfaceEXT(VkInstance instance,
	const VkHeadlessSurfaceCreateInfoEXT *info,
	const VkAllocationCallbacks *allocator, VkSurfaceKHR *pSurface)
{
	InstanceData *data = InstanceOf(instance);
	if (data == nullptr || data->CreateHeadlessSurfaceEXT == nullptr)
		return VK_ERROR_EXTENSION_NOT_PRESENT;

	VkSurfaceKHR real = VK_NULL_HANDLE;
	VkResult result = data->CreateHeadlessSurfaceEXT(instance, info, allocator, &real);
	if (result != VK_SUCCESS)
		return result;

	RengaSurface *surface = new (std::nothrow) RengaSurface(real);
	if (surface == nullptr) {
		data->DestroySurfaceKHR(instance, real, allocator);
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	}

	// The handle is the address of the VKLayerSurfaceBase subobject, which
	// is the object itself (single inheritance), as applications expect.
	VkSurfaceKHR handle = (VkSurfaceKHR)(VKLayerSurfaceBase *)surface;
	{
		std::lock_guard<std::mutex> lock(sLock);
		sSurfaces[(uint64_t)handle] = surface;
	}
	*pSurface = handle;
	TRACE("vkCreateHeadlessSurfaceEXT -> %p\n", (void *)surface);
	return VK_SUCCESS;
}


static void VKAPI_CALL
Layer_DestroySurfaceKHR(VkInstance instance, VkSurfaceKHR handle,
	const VkAllocationCallbacks *allocator)
{
	InstanceData *data = InstanceOf(instance);
	if (data == nullptr)
		return;

	RengaSurface *surface = nullptr;
	{
		std::lock_guard<std::mutex> lock(sLock);
		auto it = sSurfaces.find((uint64_t)handle);
		if (it != sSurfaces.end()) {
			surface = it->second;
			sSurfaces.erase(it);
		}
	}
	if (surface == nullptr) {
		data->DestroySurfaceKHR(instance, handle, allocator);
		return;
	}
	data->DestroySurfaceKHR(instance, surface->real, allocator);
	delete surface;
}


static void
ApplyWindowExtent(RengaSurface *surface, VkSurfaceCapabilitiesKHR &caps)
{
	if (surface == nullptr || surface->hook == nullptr)
		return;
	uint32_t width = 0, height = 0;
	surface->hook->GetSize(width, height);
	if (width == 0 || height == 0)
		return;
	caps.currentExtent = {width, height};
	caps.minImageExtent = {
		std::min(caps.minImageExtent.width, width),
		std::min(caps.minImageExtent.height, height)};
	caps.maxImageExtent = {
		std::max(caps.maxImageExtent.width, width),
		std::max(caps.maxImageExtent.height, height)};
}


static VkResult VKAPI_CALL
Layer_GetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice physicalDevice,
	uint32_t queueFamilyIndex, VkSurfaceKHR surface, VkBool32 *pSupported)
{
	InstanceData *data = InstanceOf(physicalDevice);
	return data->GetPhysicalDeviceSurfaceSupportKHR(physicalDevice,
		queueFamilyIndex, RealSurface(surface), pSupported);
}


static VkResult VKAPI_CALL
Layer_GetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice physicalDevice,
	VkSurfaceKHR handle, VkSurfaceCapabilitiesKHR *caps)
{
	InstanceData *data = InstanceOf(physicalDevice);
	RengaSurface *surface = SurfaceOf(handle);
	VkResult result = data->GetPhysicalDeviceSurfaceCapabilitiesKHR(
		physicalDevice, RealSurface(handle), caps);
	if (result == VK_SUCCESS)
		ApplyWindowExtent(surface, *caps);
	return result;
}


static VkResult VKAPI_CALL
Layer_GetPhysicalDeviceSurfaceCapabilities2KHR(VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *caps)
{
	InstanceData *data = InstanceOf(physicalDevice);
	if (data->GetPhysicalDeviceSurfaceCapabilities2KHR == nullptr)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	RengaSurface *surface = SurfaceOf(info->surface);
	VkPhysicalDeviceSurfaceInfo2KHR down = *info;
	down.surface = RealSurface(info->surface);
	VkResult result = data->GetPhysicalDeviceSurfaceCapabilities2KHR(
		physicalDevice, &down, caps);
	if (result == VK_SUCCESS)
		ApplyWindowExtent(surface, caps->surfaceCapabilities);
	return result;
}


static bool
ShowableFormat(VkFormat format)
{
	return format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}


// Two-call idiom over a filtered list: query all, keep the showable ones,
// then copy out as many as the caller has room for.
template<typename T, typename Query, typename FormatOf>
static VkResult
FilteredFormats(Query query, FormatOf formatOf, const T &templ,
	uint32_t *pCount, T *pFormats)
{
	uint32_t count = 0;
	VkResult result = query(&count, nullptr);
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return result;
	std::vector<T> all(count, templ);
	result = query(&count, all.data());
	if (result != VK_SUCCESS && result != VK_INCOMPLETE)
		return result;
	all.resize(count);
	all.erase(std::remove_if(all.begin(), all.end(),
		[&](const T &f) { return !ShowableFormat(formatOf(f)); }), all.end());

	if (pFormats == nullptr) {
		*pCount = (uint32_t)all.size();
		return VK_SUCCESS;
	}
	uint32_t n = std::min(*pCount, (uint32_t)all.size());
	for (uint32_t i = 0; i < n; i++) {
		// Keep the caller's sType/pNext for the *2 structs.
		void *callerNext = nullptr;
		if constexpr (std::is_same_v<T, VkSurfaceFormat2KHR>)
			callerNext = pFormats[i].pNext;
		pFormats[i] = all[i];
		if constexpr (std::is_same_v<T, VkSurfaceFormat2KHR>)
			pFormats[i].pNext = callerNext;
	}
	*pCount = n;
	return n < all.size() ? VK_INCOMPLETE : VK_SUCCESS;
}


static VkResult VKAPI_CALL
Layer_GetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice physicalDevice,
	VkSurfaceKHR handle, uint32_t *pCount, VkSurfaceFormatKHR *pFormats)
{
	InstanceData *data = InstanceOf(physicalDevice);
	VkSurfaceKHR real = RealSurface(handle);
	if (SurfaceOf(handle) == nullptr)
		return data->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, real,
			pCount, pFormats);
	return FilteredFormats<VkSurfaceFormatKHR>(
		[&](uint32_t *c, VkSurfaceFormatKHR *f) {
			return data->GetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, real, c, f);
		},
		[](const VkSurfaceFormatKHR &f) { return f.format; },
		VkSurfaceFormatKHR {}, pCount, pFormats);
}


static VkResult VKAPI_CALL
Layer_GetPhysicalDeviceSurfaceFormats2KHR(VkPhysicalDevice physicalDevice,
	const VkPhysicalDeviceSurfaceInfo2KHR *info, uint32_t *pCount,
	VkSurfaceFormat2KHR *pFormats)
{
	InstanceData *data = InstanceOf(physicalDevice);
	if (data->GetPhysicalDeviceSurfaceFormats2KHR == nullptr)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	VkPhysicalDeviceSurfaceInfo2KHR down = *info;
	down.surface = RealSurface(info->surface);
	if (SurfaceOf(info->surface) == nullptr)
		return data->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &down,
			pCount, pFormats);
	// Extension structs the caller chained on its entries are not passed
	// down here; none of them affect which formats exist.
	VkSurfaceFormat2KHR templ {};
	templ.sType = VK_STRUCTURE_TYPE_SURFACE_FORMAT_2_KHR;
	return FilteredFormats<VkSurfaceFormat2KHR>(
		[&](uint32_t *c, VkSurfaceFormat2KHR *f) {
			return data->GetPhysicalDeviceSurfaceFormats2KHR(physicalDevice, &down, c, f);
		},
		[](const VkSurfaceFormat2KHR &f) { return f.surfaceFormat.format; },
		templ, pCount, pFormats);
}


static VkResult VKAPI_CALL
Layer_GetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice physicalDevice,
	VkSurfaceKHR surface, uint32_t *pCount, VkPresentModeKHR *pModes)
{
	InstanceData *data = InstanceOf(physicalDevice);
	return data->GetPhysicalDeviceSurfacePresentModesKHR(physicalDevice,
		RealSurface(surface), pCount, pModes);
}


static VkResult VKAPI_CALL
Layer_GetPhysicalDevicePresentRectanglesKHR(VkPhysicalDevice physicalDevice,
	VkSurfaceKHR surface, uint32_t *pCount, VkRect2D *pRects)
{
	InstanceData *data = InstanceOf(physicalDevice);
	if (data->GetPhysicalDevicePresentRectanglesKHR == nullptr)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	return data->GetPhysicalDevicePresentRectanglesKHR(physicalDevice,
		RealSurface(surface), pCount, pRects);
}


// #pragma mark - swapchains


// The create info as passed down: the real surface, and the hook chained
// in front of the application's pNext.
struct DownSwapchainInfo {
	VkSwapchainCreateInfoKHR info;
	wsi_renga_swapchain_bitmap_hook hook;

	explicit DownSwapchainInfo(const VkSwapchainCreateInfoKHR &in)
		: info(in), hook {}
	{
		RengaSurface *surface = SurfaceOf(in.surface);
		info.surface = RealSurface(in.surface);
		if (surface != nullptr && surface->hook != nullptr) {
			hook.sType = WSI_RENGA_STRUCTURE_TYPE_SWAPCHAIN_BITMAP_HOOK;
			hook.pNext = in.pNext;
			hook.bitmap_hook = surface->hook;
			info.pNext = &hook;
		}
		TRACE("vkCreateSwapchainKHR %ux%u, %u images, mode %d, hook %p\n",
			in.imageExtent.width, in.imageExtent.height, in.minImageCount,
			in.presentMode, surface != nullptr ? (void *)surface->hook : nullptr);
	}

	DownSwapchainInfo(const DownSwapchainInfo &) = delete;
};


static VkResult VKAPI_CALL
Layer_CreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
	const VkAllocationCallbacks *allocator, VkSwapchainKHR *pSwapchain)
{
	DeviceData *data = DeviceOf(device);
	DownSwapchainInfo down(*info);
	return data->CreateSwapchainKHR(device, &down.info, allocator, pSwapchain);
}


static VkResult VKAPI_CALL
Layer_CreateSharedSwapchainsKHR(VkDevice device, uint32_t count,
	const VkSwapchainCreateInfoKHR *infos,
	const VkAllocationCallbacks *allocator, VkSwapchainKHR *pSwapchains)
{
	DeviceData *data = DeviceOf(device);
	if (data->CreateSharedSwapchainsKHR == nullptr)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	std::vector<DownSwapchainInfo *> downs;
	std::vector<VkSwapchainCreateInfoKHR> list;
	for (uint32_t i = 0; i < count; i++) {
		downs.push_back(new DownSwapchainInfo(infos[i]));
		list.push_back(downs.back()->info);
		// list[i].pNext still points into downs[i], which outlives the call.
	}
	VkResult result = data->CreateSharedSwapchainsKHR(device, count,
		list.data(), allocator, pSwapchains);
	for (DownSwapchainInfo *d : downs)
		delete d;
	return result;
}


static VkResult VKAPI_CALL
Layer_GetDeviceGroupSurfacePresentModesKHR(VkDevice device,
	VkSurfaceKHR surface, VkDeviceGroupPresentModeFlagsKHR *pModes)
{
	DeviceData *data = DeviceOf(device);
	if (data->GetDeviceGroupSurfacePresentModesKHR == nullptr)
		return VK_ERROR_EXTENSION_NOT_PRESENT;
	return data->GetDeviceGroupSurfacePresentModesKHR(device,
		RealSurface(surface), pModes);
}


// #pragma mark - instance and device


template<typename T>
static const T *
FindLayerInfo(const void *pNext, VkStructureType sType, VkLayerFunction function)
{
	for (const VkBaseInStructure *s = (const VkBaseInStructure *)pNext; s != nullptr;
			s = s->pNext) {
		if (s->sType == sType && ((const T *)s)->function == function)
			return (const T *)s;
	}
	return nullptr;
}


static VkResult VKAPI_CALL
Layer_CreateInstance(const VkInstanceCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkInstance *pInstance)
{
	auto *link = const_cast<VkLayerInstanceCreateInfo *>(
		FindLayerInfo<VkLayerInstanceCreateInfo>(info->pNext,
			VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO, VK_LAYER_LINK_INFO));
	if (link == nullptr || link->u.pLayerInfo == nullptr)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	// Advance the chain for the next layer before calling down.
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;

	auto createInstance = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
	VkResult result = createInstance(info, allocator, pInstance);
	if (result != VK_SUCCESS)
		return result;

	InstanceData *data = new (std::nothrow) InstanceData {};
	if (data == nullptr)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkInstance instance = *pInstance;
	data->instance = instance;
	data->gipa = gipa;
#define GET(name) data->name = (PFN_vk##name)gipa(instance, "vk" #name)
	GET(DestroyInstance);
	GET(CreateHeadlessSurfaceEXT);
	GET(DestroySurfaceKHR);
	GET(GetPhysicalDeviceSurfaceSupportKHR);
	GET(GetPhysicalDeviceSurfaceCapabilitiesKHR);
	GET(GetPhysicalDeviceSurfaceCapabilities2KHR);
	GET(GetPhysicalDeviceSurfaceFormatsKHR);
	GET(GetPhysicalDeviceSurfaceFormats2KHR);
	GET(GetPhysicalDeviceSurfacePresentModesKHR);
	GET(GetPhysicalDevicePresentRectanglesKHR);
#undef GET

	std::lock_guard<std::mutex> lock(sLock);
	sInstances[KeyOf(instance)] = data;
	return VK_SUCCESS;
}


static void VKAPI_CALL
Layer_DestroyInstance(VkInstance instance, const VkAllocationCallbacks *allocator)
{
	InstanceData *data = nullptr;
	{
		std::lock_guard<std::mutex> lock(sLock);
		auto it = sInstances.find(KeyOf(instance));
		if (it == sInstances.end())
			return;
		data = it->second;
		sInstances.erase(it);
	}
	data->DestroyInstance(instance, allocator);
	delete data;
}


static VkResult VKAPI_CALL
Layer_CreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo *info,
	const VkAllocationCallbacks *allocator, VkDevice *pDevice)
{
	auto *link = const_cast<VkLayerDeviceCreateInfo *>(
		FindLayerInfo<VkLayerDeviceCreateInfo>(info->pNext,
			VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO, VK_LAYER_LINK_INFO));
	if (link == nullptr || link->u.pLayerInfo == nullptr)
		return VK_ERROR_INITIALIZATION_FAILED;

	PFN_vkGetInstanceProcAddr gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
	PFN_vkGetDeviceProcAddr gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;
	link->u.pLayerInfo = link->u.pLayerInfo->pNext;

	InstanceData *instanceData = InstanceOf(physicalDevice);
	auto createDevice = (PFN_vkCreateDevice)gipa(
		instanceData != nullptr ? instanceData->instance : VK_NULL_HANDLE,
		"vkCreateDevice");
	VkResult result = createDevice(physicalDevice, info, allocator, pDevice);
	if (result != VK_SUCCESS)
		return result;

	DeviceData *data = new (std::nothrow) DeviceData {};
	if (data == nullptr)
		return VK_ERROR_OUT_OF_HOST_MEMORY;
	VkDevice device = *pDevice;
	data->device = device;
	data->gdpa = gdpa;
#define GET(name) data->name = (PFN_vk##name)gdpa(device, "vk" #name)
	GET(DestroyDevice);
	GET(CreateSwapchainKHR);
	GET(CreateSharedSwapchainsKHR);
	GET(GetDeviceGroupSurfacePresentModesKHR);
#undef GET

	std::lock_guard<std::mutex> lock(sLock);
	sDevices[KeyOf(device)] = data;
	return VK_SUCCESS;
}


static void VKAPI_CALL
Layer_DestroyDevice(VkDevice device, const VkAllocationCallbacks *allocator)
{
	DeviceData *data = nullptr;
	{
		std::lock_guard<std::mutex> lock(sLock);
		auto it = sDevices.find(KeyOf(device));
		if (it == sDevices.end())
			return;
		data = it->second;
		sDevices.erase(it);
	}
	data->DestroyDevice(device, allocator);
	delete data;
}


// #pragma mark - proc addr


#define ENTRY(name) {"vk" #name, (PFN_vkVoidFunction)Layer_##name}

struct Entry {
	const char *name;
	PFN_vkVoidFunction function;
};

static const Entry kInstanceEntries[] = {
	ENTRY(CreateInstance),
	ENTRY(DestroyInstance),
	ENTRY(CreateDevice),
	ENTRY(CreateHeadlessSurfaceEXT),
	ENTRY(DestroySurfaceKHR),
	ENTRY(GetPhysicalDeviceSurfaceSupportKHR),
	ENTRY(GetPhysicalDeviceSurfaceCapabilitiesKHR),
	ENTRY(GetPhysicalDeviceSurfaceCapabilities2KHR),
	ENTRY(GetPhysicalDeviceSurfaceFormatsKHR),
	ENTRY(GetPhysicalDeviceSurfaceFormats2KHR),
	ENTRY(GetPhysicalDeviceSurfacePresentModesKHR),
	ENTRY(GetPhysicalDevicePresentRectanglesKHR),
};

static const Entry kDeviceEntries[] = {
	ENTRY(DestroyDevice),
	ENTRY(CreateSwapchainKHR),
	ENTRY(CreateSharedSwapchainsKHR),
	ENTRY(GetDeviceGroupSurfacePresentModesKHR),
};

#undef ENTRY


static PFN_vkVoidFunction
Find(const Entry *entries, size_t count, const char *name)
{
	for (size_t i = 0; i < count; i++) {
		if (strcmp(entries[i].name, name) == 0)
			return entries[i].function;
	}
	return nullptr;
}


LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
RengaWsi_GetDeviceProcAddr(VkDevice device, const char *name)
{
	PFN_vkVoidFunction f = Find(kDeviceEntries,
		sizeof(kDeviceEntries) / sizeof(kDeviceEntries[0]), name);
	if (f != nullptr)
		return f;
	DeviceData *data = DeviceOf(device);
	return data != nullptr ? data->gdpa(device, name) : nullptr;
}


LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
RengaWsi_GetInstanceProcAddr(VkInstance instance, const char *name)
{
	PFN_vkVoidFunction f = Find(kInstanceEntries,
		sizeof(kInstanceEntries) / sizeof(kInstanceEntries[0]), name);
	if (f != nullptr)
		return f;
	f = Find(kDeviceEntries, sizeof(kDeviceEntries) / sizeof(kDeviceEntries[0]), name);
	if (f != nullptr)
		return f;
	if (strcmp(name, "vkGetDeviceProcAddr") == 0)
		return (PFN_vkVoidFunction)RengaWsi_GetDeviceProcAddr;
	if (instance == VK_NULL_HANDLE)
		return nullptr;
	InstanceData *data = InstanceOf(instance);
	return data != nullptr ? data->gipa(instance, name) : nullptr;
}


// Loader-layer interface version 2: the loader finds the two functions
// above through this.
LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
RengaWsi_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *iface)
{
	if (iface == nullptr || iface->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT)
		return VK_ERROR_INITIALIZATION_FAILED;
	if (iface->loaderLayerInterfaceVersion > 2)
		iface->loaderLayerInterfaceVersion = 2;
	iface->pfnGetInstanceProcAddr = RengaWsi_GetInstanceProcAddr;
	iface->pfnGetDeviceProcAddr = RengaWsi_GetDeviceProcAddr;
	iface->pfnGetPhysicalDeviceProcAddr = nullptr;
	return VK_SUCCESS;
}
