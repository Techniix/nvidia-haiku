# Requests to Renga

Things Quasar needs from Renga (`Techniix/haiku`, `ce/main`). This file is
read by the Renga work to plan its side; Quasar's branch never changes that
repository. Each request states what is needed, the proposed interface and
the reason. A request is a proposal until the Renga side accepts it.

States: **Draft** (not ready to act on), **Proposed**, **Scheduled** or
**Accepted** (with the Renga batch), **Landed** (with the Renga commit).

## Renga's reply of 2026-11-11

- R-1: file-descriptor passing to app_server (`SCM_RIGHTS` on a local socket
  alongside the app_server link) and the header's home and names come in
  batch F-58. The proposed `<add-ons/graphics/GpuPresent.h>` and table layout
  are the starting point; changes will be answered in this file. The
  app_server side (GPU buffers in overlay bitmaps, the flip queue, events on
  epoll) is on the F-47 checklist for the GPU stage. Quasar is to settle the
  three open points on the RTX 5070 before then.
- R-2: GPU stage, with R-1. OBS Studio asks for the same (its O-10) and
  needs, per frame, a handle importable with EGL or Vulkan external memory,
  the damaged region and a timestamp.
- R-3: batch F-49. R-4: with game mode, F-50; Renga proposes the interface
  here once the service design is fixed. R-5: accepted in principle for the
  GPU stage, because it changes how area and team teardown treat wired
  pages; the extension stays unadvertised until then.
- Media Kit phase 5 (`446221a6`) added a hardware video interface,
  `headers/private/media/VideoAcceleration.h`. Back ends are add-ons in
  `add-ons/media/video_accelerators` exporting
  `instantiate_video_accelerator(version)`; decode takes whole compressed
  packets; surfaces are NV12 or P010; the FFmpeg plug-in uses it. A Quasar
  back end must match `B_VIDEO_ACCELERATION_VERSION`. Whether it needs to be
  a public header is open (see "Open questions to Renga").

## Open questions to Renga

- VideoAcceleration.h: a back end shipped in Quasar's package, not built with
  the system, needs the header and a stable version. Request: install it with
  the development headers (it may stay private API), and bump
  `B_VIDEO_ACCELERATION_VERSION` on any change of layout.

| ID | Request | State | Roadmap entry |
| --- | --- | --- | --- |
| R-1 | Vendor-neutral accelerant extension: buffer import/export, page flip with completion events, vblank events | Accepted: F-58 (fd passing, header home), F-47 GPU stage (app_server) | F-35, F-47, F-58 |
| R-2 | Scanout and window buffer export for privileged capture clients | Scheduled: F-47 GPU stage, with R-1 | F-47, F-31 |
| R-3 | `futex()` wait/wake | Scheduled: F-49 | F-49 |
| R-4 | Game mode notifications for drivers | Scheduled: F-50 (Renga proposes the interface) | F-50 |
| R-5 | Long-term pinning of user pages for device DMA | Accepted in principle: GPU stage | F-35 (NVK coverage), F-48 |

## R-1 Accelerant extension

Proposed as a first draft, so the Renga side can start on app_server while
the driver half is written. Nothing below is implemented or tested; the
"Open points" must be settled on the RTX 5070 before the interface is fixed.

**What the hardware path offers (from NVKMS's client interface,
`nvkms-api.h`, read on 2026-11-11):**

- Surfaces are registered from RM memory (`NVKMS_IOCTL_REGISTER_SURFACE`),
  by RM handle or by file descriptor, and can be passed between NVKMS
  clients (`GRANT_SURFACE` / `ACQUIRE_SURFACE`, by descriptor).
- `NVKMS_IOCTL_FLIP` flips per head and per layer; layers can be positioned
  (`SET_LAYER_POSITION`).
- Events are queued per client (`DECLARE_EVENT_INTEREST`, `GET_NEXT_EVENT`),
  and Quasar's modeset device already supports `select()` on that queue
  (`KmsDevice.cpp`). A flip-occurred event type exists.
- `NVKMS_IOCTL_NOTIFY_VBLANK` signals a descriptor at the next vblank of a
  head (one-shot, re-armed by the caller).
- A VRR semaphore surface can be exported and signalled.

**Proposed interface.** One new accelerant feature code returns a table of
functions. All calls are made by app_server (the accelerant's owner).

```c
// <add-ons/graphics/GpuPresent.h> (name provisional)
#define B_GPU_PRESENT_HOOKS  0x52474100  // vendor-neutral feature code

enum { B_GPU_BUFFER_FD = 1 };   // handle is a file descriptor

typedef struct {
	uint32  handle_type;        // B_GPU_BUFFER_FD
	int     handle;             // driver-specific shareable descriptor
	uint32  width, height;
	uint32  bytes_per_row;      // 0 if not linear
	color_space format;
	uint64  modifier;           // driver tiling/layout, 0 = linear
	uint64  offset;             // of the image inside the object
} gpu_buffer_desc;

typedef struct {
	uint32  planes_per_head;    // 1 = primary only
	uint32  flags;              // B_GPU_PRESENT_VRR, _ASYNC_FLIP, _SCALING...
	uint32  max_width, max_height;
} gpu_present_caps;

typedef struct {
	gpu_rect source;            // {x, y, width, height}, buffer pixels
	gpu_rect destination;       // on screen; ignored for the primary plane
	uint32  flags;              // B_GPU_FLIP_VSYNC, _ASYNC, _VRR
	int     wait_fd;            // -1, or a descriptor that becomes readable
	                            // when rendering to the buffer is complete
	uint64  user_data;          // returned in the completion event
} gpu_flip_params;

enum { B_GPU_EVENT_FLIP_COMPLETE = 1, B_GPU_EVENT_VBLANK = 2 };

typedef struct {
	uint32  type;
	uint32  head;
	uint32  plane;
	uint64  user_data;
	bigtime_t time;             // system_time() of the event
	uint64  sequence;           // vblank count
} gpu_present_event;

typedef struct {
	uint32   version;
	status_t (*get_caps)(uint32 head, gpu_present_caps* caps);
	status_t (*import_buffer)(const gpu_buffer_desc* desc, uint32* _id);
	status_t (*release_buffer)(uint32 id);
	status_t (*flip)(uint32 head, uint32 plane, uint32 id,
				const gpu_flip_params* params);
	status_t (*disable_plane)(uint32 head, uint32 plane);
	status_t (*request_vblank)(uint32 head, bool repeat);
	int      (*event_fd)(void);  // readable while events are queued;
	                             // usable with select, poll and epoll
	status_t (*next_event)(gpu_present_event* event);
	// R-2: the current scanout buffer of a head, as a new descriptor.
	status_t (*export_scanout)(uint32 head, gpu_buffer_desc* desc);
} gpu_present_hooks;
```

**Why these choices:**

- *A file descriptor as the buffer handle.* It is the one handle both driver
  families can share between processes: Quasar's NVK already exports memory
  as an `nvidiactl` descriptor through RM, and LinuxKPI DRM drivers have
  dma-buf descriptors. The client passes it to app_server, which needs a way
  to receive a descriptor from a client (see "Needed from Renga").
- *An event descriptor, not a callback.* app_server waits on it in an
  existing thread together with its other sources, and Renga has `epoll`.
- *Planes.* Windowed zero-copy needs an overlay plane with a destination
  rectangle; fullscreen direct scanout is a flip on plane 0.

**Needed from Renga:**

1. app_server: accept a buffer descriptor from a client for an overlay
   bitmap or a direct-scanout window, and the flip queue of F-47.
2. A way to pass a file descriptor from an application to app_server (the
   app_server link has none today; `SCM_RIGHTS` on a local socket pair would
   do).
3. A decision on the header's home and names, since the LinuxKPI DRM
   drivers implement the same table.

**Open points (need the hardware):**

- How many layers per head NVKMS reports on the RTX 5070, and whether an
  overlay layer can be positioned and scaled as windowed presentation needs.
- Whether a flip can wait on the client's rendering in the kernel
  (`wait_fd`), or whether the WSI must wait before asking for the flip.
- Flip timing on a VRR mode, and the latency from vblank to the event.

## R-2 Buffer export for capture

Scheduled. A privileged client (Sunshine, OBS, the Media Kit capture API) obtains
the current scanout buffer or a window's buffer as a GPU handle. The driver
side is `export_scanout()` in R-1's table, returning a descriptor the client
imports into Vulkan. Renga decides who may ask, and how a window's buffer is
named. OBS also needs the damaged region and a timestamp per frame; the
timestamp can come from the flip-complete event, the damage only from
app_server.

## R-3 `futex()`

No change requested: F-49 as designed (private and shared, timeouts) covers
Mesa's `util/futex`. Quasar adds the Mesa case once it lands.

## R-4 Game mode notifications

Draft. Quasar should raise the GPU performance level and lower present
latency while a program holds game mode. Needed: a way for a driver or its
user-space service to learn which teams hold game mode and when that
changes. The interface is to be proposed once F-50's service design is
fixed.

## R-5 Long-term pinning of user pages

Proposed.

**Needed for:** `VK_EXT_external_memory_host` (on Eglantine's list). NVK implements it with RM's
`NV01_MEMORY_SYSTEM_OS_DESCRIPTOR`: RM asks the OS layer to pin a range of the
calling team's memory (`os_lock_user_pages()`), maps those physical pages into
the GPU's address space, and unpins them when the memory object is freed,
which may be when the client's `nvidiactl` descriptor is closed during team
teardown.

**What is missing:** `lock_memory_etc()` wires pages for the duration of an
I/O operation. Quasar needs a pin that:

1. keeps the physical pages allocated and at the same address until the
   driver releases them, even if the application unmaps or resizes the area
   or the team exits first (the device may still be reading or writing);
2. can be released from any thread, including during team teardown, without
   the team's address space still existing;
3. reports the physical address of each page;
4. breaks copy-on-write when pinning for write, so the device and the
   application see the same pages.

On Linux this is `pin_user_pages(FOLL_LONGTERM)`.

**Proposed interface** (kernel, private header):

```c
// Pins [address, address + size) of the given team for device access.
// On success *_cookie identifies the pin and physicalPages[0..size /
// B_PAGE_SIZE) holds the page addresses.
status_t vm_pin_user_pages(team_id team, void* address, size_t size,
	bool writable, phys_addr_t* physicalPages, void** _cookie);

// Releases a pin from any context that may block. Safe after the team or
// the area is gone.
void vm_unpin_user_pages(void* cookie);
```

A possible implementation holds a reference on each `vm_page` (wired count
plus a cache reference) so that deleting the area leaves the pages alive
until the last unpin. The open question for the Renga side is how area and
address-space teardown treat wired pages today; Quasar does not attempt this
with `lock_memory_etc()` because a wrong guess there is a kernel panic at
application exit.

**Until then:** the allocation fails with `NV_ERR_NOT_SUPPORTED`, logged once,
and NVK does not advertise the extension.

## Upstream header bug: `user_fd_kernel_ioctl()`

Not a request, a note for whoever touches it. `headers/private/kernel/fs/fd.h`
declares `user_fd_kernel_ioctl(int, ulong, void*, size_t)` inside
`extern "C"`, but `src/system/kernel/fs/fd.cpp` defines it in C++ with
`uint32 op`, which is a different, C++-mangled symbol. Code that uses the
header links against nothing. Quasar declares the C++ form itself
(`BaseDevice.cpp`, `KmsDevice.cpp`). If the definition is fixed to match the
header (C linkage), Quasar's two declarations must change in the same step,
or Quasar stops linking. The same mismatch is likely in stock Haiku.
