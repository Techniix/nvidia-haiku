/*
 * Host test for RengaWsi (no GPU, no Haiku): the layer is linked into this
 * program and driven through the loader's layer interface, with a fake
 * "next" layer underneath that records what reaches it.
 *
 * Checks:
 *  - the surface handle can be cast to VKLayerSurfaceBase and
 *    SetBitmapHook() called on it, as zink_kopper_haiku.cpp does;
 *  - surface queries reach the driver with the real surface;
 *  - currentExtent follows the hook's GetSize();
 *  - surface formats are filtered to BGRA8, with the two-call idiom and
 *    VK_INCOMPLETE;
 *  - vkCreateSwapchainKHR reaches the driver with the real surface and the
 *    bitmap-hook struct chained in front of the application's pNext;
 *  - a surface without a hook gets no struct;
 *  - destroying the surface destroys the real one.
 */
#include <stdio.h>
#include <string.h>
#include <vector>

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>

#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)

class BBitmap;
class BitmapHook {
public:
	virtual ~BitmapHook() {};
	virtual void GetSize(uint32_t &width, uint32_t &height) = 0;
	virtual BBitmap *SetBitmap(BBitmap *bmp) = 0;
};
class VKLayerSurfaceBase {
public:
	virtual ~VKLayerSurfaceBase() {};
	virtual void SetBitmapHook(BitmapHook *hook) = 0;
};

class TestHook: public BitmapHook {
public:
	uint32_t w = 1280, h = 720;
	void GetSize(uint32_t &width, uint32_t &height) override { width = w; height = h; }
	BBitmap *SetBitmap(BBitmap *) override { return nullptr; }
};

extern "C" VkResult RengaWsi_NegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *);

// ---- fake next layer / driver ----
struct FakeDispatch { void *table; };
static FakeDispatch gInstance {(void *)0x1111}, gPhys {(void *)0x1111}, gDevice {(void *)0x2222};
static const VkSurfaceKHR kReal = (VkSurfaceKHR)0xABCD0000;
static VkSurfaceKHR gLastSurface;
// What the driver saw in vkCreateSwapchainKHR, recorded during the call
// (the layer's copy of the create info lives on its stack).
struct HookInfo { VkStructureType sType; const void *pNext; void *bitmap_hook; };
static struct {
	VkSurfaceKHR surface;
	const void *pNext;
	bool hasHook;
	HookInfo hook;
} gSeen;
static int gDestroyed;

static VkResult VKAPI_CALL F_CreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *, VkInstance *p) { *p = (VkInstance)&gInstance; return VK_SUCCESS; }
static void VKAPI_CALL F_DestroyInstance(VkInstance, const VkAllocationCallbacks *) {}
static VkResult VKAPI_CALL F_CreateHeadless(VkInstance, const VkHeadlessSurfaceCreateInfoEXT *, const VkAllocationCallbacks *, VkSurfaceKHR *s) { *s = kReal; return VK_SUCCESS; }
static void VKAPI_CALL F_DestroySurface(VkInstance, VkSurfaceKHR s, const VkAllocationCallbacks *) { if (s == kReal) gDestroyed++; }
static VkResult VKAPI_CALL F_Caps(VkPhysicalDevice, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
	gLastSurface = s; *c = {}; c->minImageCount = 2; c->maxImageCount = 0;
	c->currentExtent = {0xFFFFFFFF, 0xFFFFFFFF}; c->minImageExtent = {1, 1}; c->maxImageExtent = {16384, 16384};
	return VK_SUCCESS; }
static VkResult VKAPI_CALL F_Formats(VkPhysicalDevice, VkSurfaceKHR s, uint32_t *n, VkSurfaceFormatKHR *f) {
	gLastSurface = s;
	static const VkSurfaceFormatKHR all[] = {
		{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
		{VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
		{VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR},
		{VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}};
	if (!f) { *n = 4; return VK_SUCCESS; }
	uint32_t k = *n < 4 ? *n : 4; memcpy(f, all, k * sizeof(*f)); *n = k; return k < 4 ? VK_INCOMPLETE : VK_SUCCESS; }
static VkResult VKAPI_CALL F_Support(VkPhysicalDevice, uint32_t, VkSurfaceKHR s, VkBool32 *b) { gLastSurface = s; *b = VK_TRUE; return VK_SUCCESS; }
static VkResult VKAPI_CALL F_CreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *, const VkAllocationCallbacks *, VkDevice *d) { *d = (VkDevice)&gDevice; return VK_SUCCESS; }
static void VKAPI_CALL F_DestroyDevice(VkDevice, const VkAllocationCallbacks *) {}
static VkResult VKAPI_CALL F_CreateSwapchain(VkDevice, const VkSwapchainCreateInfoKHR *i, const VkAllocationCallbacks *, VkSwapchainKHR *s) {
	gSeen = {};
	gSeen.surface = i->surface;
	gSeen.pNext = i->pNext;
	auto *h = (const HookInfo *)i->pNext;
	if (h != nullptr && h->sType == (VkStructureType)0x52474102) {
		gSeen.hasHook = true;
		gSeen.hook = *h;
	}
	*s = (VkSwapchainKHR)0x5555; return VK_SUCCESS; }

static PFN_vkVoidFunction VKAPI_CALL F_GIPA(VkInstance, const char *n) {
#define R(name, f) if (!strcmp(n, name)) return (PFN_vkVoidFunction)f
	R("vkCreateInstance", F_CreateInstance); R("vkDestroyInstance", F_DestroyInstance);
	R("vkCreateHeadlessSurfaceEXT", F_CreateHeadless); R("vkDestroySurfaceKHR", F_DestroySurface);
	R("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", F_Caps); R("vkGetPhysicalDeviceSurfaceFormatsKHR", F_Formats);
	R("vkGetPhysicalDeviceSurfaceSupportKHR", F_Support); R("vkCreateDevice", F_CreateDevice);
	return nullptr; }
static PFN_vkVoidFunction VKAPI_CALL F_GDPA(VkDevice, const char *n) {
	R("vkDestroyDevice", F_DestroyDevice); R("vkCreateSwapchainKHR", F_CreateSwapchain);
	return nullptr;
#undef R
}

int main()
{
	VkNegotiateLayerInterface neg {LAYER_NEGOTIATE_INTERFACE_STRUCT, nullptr, 2};
	CHECK(RengaWsi_NegotiateLoaderLayerInterfaceVersion(&neg) == VK_SUCCESS);
	auto gipa = neg.pfnGetInstanceProcAddr;
	auto gdpa = neg.pfnGetDeviceProcAddr;

	VkLayerInstanceLink ilink {nullptr, F_GIPA, nullptr};
	VkLayerInstanceCreateInfo ici {};
	ici.sType = VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO;
	ici.function = VK_LAYER_LINK_INFO;
	ici.u.pLayerInfo = &ilink;
	VkInstanceCreateInfo info {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, &ici};
	VkInstance instance;
	CHECK(((PFN_vkCreateInstance)gipa(nullptr, "vkCreateInstance"))(&info, nullptr, &instance) == VK_SUCCESS);
	VkPhysicalDevice phys = (VkPhysicalDevice)&gPhys;

	VkSurfaceKHR surface;
	VkHeadlessSurfaceCreateInfoEXT hsi {VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};
	CHECK(((PFN_vkCreateHeadlessSurfaceEXT)gipa(instance, "vkCreateHeadlessSurfaceEXT"))(instance, &hsi, nullptr, &surface) == VK_SUCCESS);
	CHECK(surface != kReal);

	// Before a hook: extent stays undefined; queries see the real surface.
	auto caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
	VkSurfaceCapabilitiesKHR c;
	CHECK(caps(phys, surface, &c) == VK_SUCCESS);
	CHECK(gLastSurface == kReal);
	CHECK(c.currentExtent.width == 0xFFFFFFFF);

	// What Zink does.
	TestHook hook;
	((VKLayerSurfaceBase *)surface)->SetBitmapHook(&hook);
	CHECK(caps(phys, surface, &c) == VK_SUCCESS);
	CHECK(c.currentExtent.width == 1280 && c.currentExtent.height == 720);
	hook.w = 640; hook.h = 480;
	CHECK(caps(phys, surface, &c) == VK_SUCCESS);
	CHECK(c.currentExtent.width == 640 && c.currentExtent.height == 480);

	VkBool32 supported = VK_FALSE;
	CHECK(((PFN_vkGetPhysicalDeviceSurfaceSupportKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"))(phys, 0, surface, &supported) == VK_SUCCESS);
	CHECK(supported && gLastSurface == kReal);

	// Formats: 4 from the driver, 2 showable.
	auto formats = (PFN_vkGetPhysicalDeviceSurfaceFormatsKHR)gipa(instance, "vkGetPhysicalDeviceSurfaceFormatsKHR");
	uint32_t n = 0;
	CHECK(formats(phys, surface, &n, nullptr) == VK_SUCCESS && n == 2);
	VkSurfaceFormatKHR f[4] {};
	n = 1;
	CHECK(formats(phys, surface, &n, f) == VK_INCOMPLETE && n == 1);
	CHECK(f[0].format == VK_FORMAT_B8G8R8A8_UNORM);
	n = 4;
	CHECK(formats(phys, surface, &n, f) == VK_SUCCESS && n == 2);
	CHECK(f[1].format == VK_FORMAT_B8G8R8A8_SRGB);
	CHECK(f[0].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR);

	// Device and swapchain.
	VkLayerDeviceLink dlink {nullptr, F_GIPA, F_GDPA};
	VkLayerDeviceCreateInfo dci {};
	dci.sType = VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO;
	dci.function = VK_LAYER_LINK_INFO;
	dci.u.pLayerInfo = &dlink;
	VkDeviceCreateInfo dinfo {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &dci};
	VkDevice device;
	CHECK(((PFN_vkCreateDevice)gipa(instance, "vkCreateDevice"))(phys, &dinfo, nullptr, &device) == VK_SUCCESS);

	VkSwapchainPresentModesCreateInfoEXT appNext {VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_MODES_CREATE_INFO_EXT};
	VkSwapchainCreateInfoKHR sci {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, &appNext};
	sci.surface = surface;
	sci.imageExtent = {640, 480};
	sci.minImageCount = 3;
	auto createSwapchain = (PFN_vkCreateSwapchainKHR)gdpa(device, "vkCreateSwapchainKHR");
	VkSwapchainKHR sc;
	CHECK(createSwapchain(device, &sci, nullptr, &sc) == VK_SUCCESS);
	CHECK(gSeen.surface == kReal);
	CHECK(gSeen.hasHook);
	CHECK(gSeen.hook.bitmap_hook == (BitmapHook *)&hook);
	CHECK(gSeen.hook.pNext == &appNext);
	CHECK(sci.surface == surface && sci.pNext == &appNext);	// caller's struct untouched

	// A surface without a hook: no struct.
	VkSurfaceKHR bare;
	CHECK(((PFN_vkCreateHeadlessSurfaceEXT)gipa(instance, "vkCreateHeadlessSurfaceEXT"))(instance, &hsi, nullptr, &bare) == VK_SUCCESS);
	sci.surface = bare;
	CHECK(createSwapchain(device, &sci, nullptr, &sc) == VK_SUCCESS);
	CHECK(!gSeen.hasHook && gSeen.pNext == &appNext && gSeen.surface == kReal);

	auto destroySurface = (PFN_vkDestroySurfaceKHR)gipa(instance, "vkDestroySurfaceKHR");
	destroySurface(instance, surface, nullptr);
	destroySurface(instance, bare, nullptr);
	CHECK(gDestroyed == 2);

	((PFN_vkDestroyDevice)gdpa(device, "vkDestroyDevice"))(device, nullptr);
	((PFN_vkDestroyInstance)gipa(instance, "vkDestroyInstance"))(instance, nullptr);
	printf("renga-wsi-layer: all checks passed\n");
	return 0;
}
