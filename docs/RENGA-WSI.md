# Renga WSI: design

Status, 2026-11-11: components 1 (the surface layer) and 2 (the Mesa
platform) are written. The layer passes a host test with a fake driver; the
Mesa platform is syntax-checked. Neither has run on Renga. Component 3 is not
started.

The Renga WSI replaces VideoStreamsWsi on the `renga` branch. It is how
Vulkan applications, and OpenGL applications through Zink, put frames on the
screen.

## What is wrong with the current path

VideoStreamsWsi is an implicit Vulkan layer that implements
`VK_EXT_headless_surface` and `VK_KHR_swapchain` itself. Per frame it:

1. waits on the CPU for the previous copy (`vkWaitForFences`) inside the
   application's `vkQueuePresentKHR`;
2. copies the image into a linear host-visible image on queue 0, which it
   took with `vkGetDeviceQueue(0, 0)`. That queue is also the application's,
   and Vulkan requires one thread at a time per queue, so the layer and the
   application race;
3. waits again, then hands a `BBitmap` (backed by that linear image's
   memory) to the application's `BitmapHook`; app_server copies that bitmap
   into the framebuffer.

Acquire has the same queue problem: to signal the application's acquire
semaphore the layer needs a queue submission, and the only queue is the
application's. NVK on the RM back end exposes exactly one queue (one family,
`queueCount` 1, no transfer family), so a layer has no legal queue of its
own.

## Decision

**Swapchains move into NVK, on Mesa's common WSI code, as on Linux. A thin
layer keeps the surface objects that existing applications depend on.**

### Why inside the driver

Mesa's `src/vulkan/wsi` is what NVK uses on Linux. It already provides:

- acquire without a queue: once an image is idle, the application's acquire
  semaphore or fence is given an already-signalled payload
  (`vk_sync_dummy`), which only code inside the driver can do;
- present modes (FIFO, MAILBOX, IMMEDIATE), a present thread, image idle
  tracking, `VK_KHR_present_id`/`present_wait`,
  `VK_EXT_swapchain_maintenance1`, `VK_KHR_swapchain_mutable_format`
  (Eglantine uses it);
- GPU blits to a linear buffer for presentation through another device or
  process (`WSI_SWAPCHAIN_BUFFER_BLIT`), and exportable images for
  zero-copy.

A layer would have to rebuild all of that and still could not signal acquire
semaphores legally on a single queue. A platform in Mesa supplies only
surface queries, swapchain creation and the per-frame present.

### Why a layer is still needed

Zink (`zink_kopper_haiku.cpp`), OBS Studio and Eglantine create a surface
with `vkCreateHeadlessSurfaceEXT`, cast the `VkSurfaceKHR` to
VideoStreamsWsi's `VKLayerSurfaceBase` and call its virtual
`SetBitmapHook()`. A surface the Vulkan loader creates is a loader structure
with no such method, so whatever owns the surface handle must be a C++
object with that vtable. Keeping that ABI means none of them has to change
for this work.

## Components

### 1. Surface layer (`RengaWsi/`, in Quasar)

An implicit instance layer, written new (VideoStreamsWsi has no licence
file, so no code is taken from it; only the `BitmapHook` and
`VKLayerSurfaceBase` interfaces are matched, for compatibility).

- `vkCreateHeadlessSurfaceEXT`: creates the real headless surface down the
  chain (the loader's), and returns a layer object with the
  `VKLayerSurfaceBase` vtable that holds it. `SetBitmapHook()` passes the
  hook to NVK (component 2).
- Every entry point that takes a surface (`vkGetPhysicalDeviceSurface*`,
  `vkCreateSwapchainKHR`, `vkDestroySurfaceKHR`) translates the layer
  object to the real surface and calls down. Nothing else is intercepted:
  swapchains, acquire and present go straight to NVK.
- A real window surface: instance extension
  **`VK_RENGA_haiku_view_surface` (provisional name)**,
  `vkCreateHaikuViewSurfaceRENGA(instance, {BView*}, ...)`. The layer
  attaches its own `BitmapHook` to the view. The name is vendor-prefixed and
  marked provisional until it is agreed with X547, as the briefing requires;
  applications should treat it as unstable.

### 2. Renga platform in Mesa's WSI (`Techniix/mesa`, branch `renga`)

`src/vulkan/wsi/wsi_common_renga.cpp`, alongside the existing headless
platform, which it extends. It is named for Renga, not Haiku: it uses
Renga-only interfaces and this Mesa branch does not support stock Haiku.
The build still selects it with `host_machine.system() == 'haiku'`, because
that is what Renga reports.

- The hook reaches Mesa in the swapchain create info, as a private struct
  (`wsi_renga_swapchain_bitmap_hook`, sType `0x52474102`) that the layer
  chains onto `VkSwapchainCreateInfoKHR`. A separate "set hook" entry point
  would not work: the loader keeps its own surface handles and gives the
  driver only its private copy, so the layer cannot name the driver's
  surface. The loader passes `pNext` chains through unchanged. Without the
  struct a headless swapchain behaves as before.
- Swapchain images are rendered by the application as usual. With a hook,
  the swapchain uses `WSI_SWAPCHAIN_BUFFER_BLIT`: on present, Mesa's common
  code records a GPU copy into a linear, host-visible buffer as part of the
  application's own submission on the application's queue, which is legal
  because the application holds that queue during `vkQueuePresentKHR`.
- A present thread waits for that copy on the GPU fence (not in the
  application's thread), wraps the buffer in a `BBitmap` (one per buffer,
  created once over the mapped memory), and passes it to `SetBitmap()`.
  FIFO paces on the hook's consumption; MAILBOX replaces the queued frame;
  IMMEDIATE hands it over at once.
- Image count and extent follow the hook's `GetSize()`; a size change makes
  the swapchain report `VK_SUBOPTIMAL_KHR`, then `VK_ERROR_OUT_OF_DATE_KHR`.

Result with today's app_server: no CPU wait in the application's thread and
no queue shared between threads. The copies stay as they are: one GPU copy
into the bitmap's memory (VideoStreamsWsi also copies straight into a
bitmap-backed image), then app_server's copy into the framebuffer. Removing
those is step 5.

### 3. Zero-copy (after Renga F-47)

When app_server accepts GPU buffers (R-1), the same platform:

- allocates exportable swapchain images (`VK_KHR_external_memory_fd`, the
  nvidiactl descriptor `NvPresent` imports);
- sends them to app_server once, through the descriptor passing of F-58;
- on present, sends the image index and a completion descriptor instead of a
  bitmap. app_server flips it onto an overlay plane (windowed) or the
  primary plane (fullscreen) with `B_GPU_PRESENT_HOOKS`.

No CPU copy remains. VRR and present timing come from the flip and vblank
events.

## Order of work

1. Mesa platform with the bitmap path (component 2). Written; testable
   without the layer by a small program that chains the struct itself.
2. Surface layer (component 1). Written, `RengaWsi/`; host test
   `tests/renga-wsi-layer`. Zink, OBS and Eglantine move over unchanged.
3. `VK_RENGA_haiku_view_surface`.
4. Remove VideoStreamsWsi from the `renga` build. Done as the default:
   `Build.sh` builds RengaWsi; `QUASAR_WSI=videostreams` (for `Build.sh` and
   `tools/make-package.sh`) builds and packages VideoStreamsWsi instead, as a
   fallback while the new path is tested. The submodule stays until the new
   path has passed on hardware.
5. Zero-copy, when F-47 and F-58 land.

## Risks

- Mesa's common WSI is new code on Haiku. Step 1 is tested on its own first,
  and VideoStreamsWsi stays installable until step 4.
- Two layers must not both claim `VK_EXT_headless_surface`. The Quasar
  package installs one of them; the test script records which is active
  (`vulkan-layers.txt`). A VideoStreamsWsi left in a home-folder add-ons
  directory from an earlier manual install would conflict and has to be
  removed.
- The private entry point is not a Vulkan extension and is not advertised.
  It exists only between the layer and NVK.
