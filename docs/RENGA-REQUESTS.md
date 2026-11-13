# Requests to Renga

Things Quasar needs from Renga (`Techniix/haiku`, `ce/main`). This file is
read by the Renga work to plan its side; Quasar's branch never changes that
repository. Each request states what is needed, the proposed interface and
the reason. A request is a proposal until the Renga side accepts it.

States: **Draft** (not ready to act on), **Proposed**, **Scheduled** or
**Accepted** (with the Renga batch), **Landed** (with the Renga commit).

## Renga's notes of 2026-11-13

- FFmpeg with Vulkan: Renga builds its own FFmpeg 6.1.6 with
  `--enable-vulkan` (RengaPorts recipe, ahead of HaikuPorts) and puts
  `vulkan_loader` in the image, right after its development-package work.
  Quasar's answer: FFmpeg 6.1 is enough. Vulkan encode (FFmpeg 7.1) isn't
  needed, because NVK has no video encode and NVENC can't be driven
  (`docs/VIDEO.md`); stay on `ffmpeg6`.
- The GPU stage moves up to third in Renga's work order, and Renga asks for
  everything else the driver needs to be feature-complete. That is R-6 to
  R-15 below, with priorities and interfaces.
- Hardware cursor: acknowledged; nothing further needed.

## Renga's notes of 2026-11-12, round 2

- `6af51a3a`: the two `GpuPresent.h` comment changes Quasar asked for are in,
  worded as Quasar implements them, and `B_GPU_PRESENT_PLACEMENT` (0x08) was
  added; `B_GPU_PRESENT_SCALING` implies it. Quasar keeps setting only
  `SCALING`. Closes that open question.
- `aa1cd0da` (F-59): drag bitmaps stay in the hardware cursor. app_server
  composites the pointer and the drag bitmap into one cursor image for
  `set_cursor_bitmap()`, and the optional `B_GET_CURSOR_MAX_SIZE` hook
  (`<add-ons/graphics/HardwareCursor.h>`) tells it the largest size the
  accelerant takes.

  Quasar's answer: done on the `renga` branch. The hardware cursor work from
  `rtx5070-hw-cursor` is merged; `set_cursor_bitmap()` takes every size up to
  256×256 (or the device's limit if lower) and refuses larger ones, and
  `B_GET_CURSOR_MAX_SIZE` reports that limit for `B_RGBA32` (other color
  spaces: `B_NOT_SUPPORTED`). The cursor test covers a drag sequence, the
  largest image and refused larger ones, against Renga's header. Not yet run
  on the RTX 5070.

## Renga's reply of 2026-11-12 (batch F-58)

- R-1, F-58 part landed in `744fa9b4`:
  - Descriptor passing: `BPrivate::app_server_send_fd(int fd, int32* token)`
    (`headers/private/app/ServerFdTransfer.h`, libbe) sends a descriptor with
    `SCM_RIGHTS` over `/boot/system/var/run/app_server/fd` and returns a
    token; only the sending team's ServerApp can take it, once; untaken
    descriptors close after 60 s or with the team. F-47's app_server messages
    will carry the token. QEMU Tested (suite `appserver_fd`).
  - The header is public as `<add-ons/graphics/GpuPresent.h>`, marked
    PROVISIONAL, with `gpu_rect`, named flag and event enums and
    `B_GPU_PRESENT_VERSION` 1 in `gpu_present_hooks::version`. Any change
    bumps the version; field changes are cheapest before F-47.
  - The app_server side stays on the F-47 checklist.
- `SOCK_SEQPACKET` UNIX sockets deliver empty messages and `MSG_CTRUNC`
  (`fcac2cd7`); the descriptor passing depends on it.
- F-58 (`975c0eff`): an older copy of a system driver in `non-packaged`
  yields to the system's, with a syslog line; a newer copy still wins. A panic
  in a driver's hooks names the driver and hook in the crash report.
- VideoAcceleration.h: already installed with the development headers, at
  `/boot/system/develop/headers/private/media/VideoAcceleration.h`, and stays
  private. Renga bumps `B_VIDEO_ACCELERATION_VERSION` on any change of layout
  or semantics, and the roster's `instantiate_video_accelerator(version)`
  call lets a back end refuse a version it doesn't implement.

### Quasar's answer (2026-11-12)

- The accelerant now builds against Renga's header: it includes
  `<add-ons/graphics/GpuPresent.h>` when the tree has it, and otherwise
  Quasar's `headers/GpuPresent.h`, now a verbatim copy of Renga's. Quasar's
  earlier code had drifted from the table proposed here (it had
  `head_count`, a `size` field and different flag names). It now follows the
  header as landed. Syntax-checked against Renga's headers; not run.
- **`gpu_buffer_desc` without `size`: fine.** NVKMS needs a size for the
  memory object. Quasar passes what the layout needs (offset plus pitch times
  height, the height rounded up to whole blocks for block-linear). RM refuses
  the display's context DMA if the object is smaller, so a bad descriptor
  fails the import rather than letting the display read past the allocation.
- **`bytes_per_row` for block-linear buffers.** The header says it is 0 when
  not linear. Quasar then derives the pitch from the width (whole 64-byte
  GOBs), which matches how NVK lays out a single-sample 2D image. A non-zero
  value is taken as the pitch in bytes. Suggested wording for the header:
  "row pitch in bytes; 0 for a non-linear buffer lets the driver derive it".
  Change requested only in the comment, not the layout.
- **`wait_fd`: implemented in the accelerant.** A flip with a descriptor is
  held, and submitted to NVKMS by the helper thread when the descriptor
  becomes readable (or reports an error, so a broken fence can't hang the
  plane). NVKMS can't wait on a file, only on semaphores in GPU memory, so
  this costs one helper-thread wakeup. Open point 2 is now a latency
  measurement on the RTX 5070, not an interface question. `disable_plane()`
  drops a flip still waiting, without an event.
- **`request_vblank(head, repeat)`: how repeating stops.** The header
  doesn't say. Quasar implements: `true` starts an event at every vblank;
  `false` asks for one event and stops repeating. A second request while one
  is outstanding doesn't add a second event. Please put that (or what you
  prefer) in the header's comment.
- **Positioning.** The draft had a flag for overlay planes that take a
  destination position; the header has only `B_GPU_PRESENT_SCALING`. Quasar
  sets `SCALING` for a plane that can be both placed and scaled (NVKMS window
  mode gives both together), so no new flag is needed for NVIDIA. A driver
  whose overlay can be placed but not scaled would need one.

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

- FFmpeg with Vulkan: answered 2026-11-13 (Renga builds FFmpeg 6.1.6 with
  `--enable-vulkan`); now R-14.

- `GpuPresent.h` comments: answered in `6af51a3a`.
- VideoAcceleration.h: answered 2026-11-12 (installed with the development
  headers; the version is bumped on any change).

| ID | Request | State | Roadmap entry |
| --- | --- | --- | --- |
| R-1 | Vendor-neutral accelerant extension: buffer import/export, page flip with completion events, vblank events | F-58 part Landed (`744fa9b4`): fd passing, header; F-47 GPU stage (app_server) Accepted | F-35, F-47, F-58 |
| R-2 | Scanout and window buffer export for privileged capture clients | Scheduled: F-47 GPU stage, with R-1 | F-47, F-31 |
| R-3 | `futex()` wait/wake | Scheduled: F-49 | F-49 |
| R-4 | Game mode notifications for drivers | Scheduled: F-50 (Renga proposes the interface) | F-50 |
| R-5 | Long-term pinning of user pages for device DMA | Accepted in principle: GPU stage | F-35 (NVK coverage), F-48 |
| R-6 | Several displays and hot-plug (display hooks, app_server screens) | Proposed, P1 | GPU stage |
| R-7 | Variable refresh rate | Proposed, P1 | GPU stage |
| R-8 | Resizable BAR in the PCI bus manager | Proposed, P1 | GPU stage |
| R-9 | Shutdown and suspend/resume for drivers | Proposed: shutdown P1, suspend P2 | GPU stage |
| R-10 | Display power: DPMS on idle, per display | Proposed, P2 | GPU stage |
| R-11 | Gamma and colour LUTs | Proposed, P2 | GPU stage |
| R-12 | Overlay planes used by app_server (video, direct scanout) | Proposed, P2 (interface is R-1) | GPU stage |
| R-13 | GPU monitoring interface (temperature, clocks, load, VRAM) | Proposed, P2 | GPU stage |
| R-14 | Hardware video decode in the Media Kit | Proposed, P2 (FFmpeg with Vulkan accepted) | GPU stage |
| R-15 | Monitor brightness (DDC/CI), HDR and 10-bit output | Proposed, P3 | later |

## R-1 Accelerant extension

The table landed as Renga's `<add-ons/graphics/GpuPresent.h>` (`744fa9b4`,
PROVISIONAL); the block below is the proposal it was taken from, and the
header is now the reference. Quasar's accelerant implements it
(syntax-checked against Renga's headers, not run). The "Open points" must
still be settled on the RTX 5070 before the interface is fixed.

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
2. A way to pass a file descriptor from an application to app_server.
   Landed: `BPrivate::app_server_send_fd()` (`744fa9b4`).
3. A decision on the header's home and names, since the LinuxKPI DRM
   drivers implement the same table. Landed:
   `<add-ons/graphics/GpuPresent.h>` (`744fa9b4`).

**Open points (need the hardware):**

- How many layers per head NVKMS reports on the RTX 5070, and whether an
  overlay layer can be positioned and scaled as windowed presentation needs.
- `wait_fd`: implemented by waiting in the accelerant's helper thread (NVKMS
  can't wait on a descriptor). To measure: the latency that adds.
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

## Feature-complete driver: R-6 to R-15 (2026-11-13)

These are designed for Renga only: where Haiku's interfaces fall short,
the request is to change Renga, not to work around it in the driver.

Priorities: **P1** a desktop driver isn't complete without it; **P2**
expected of a complete driver; **P3** later. Everything here is provisional
in the same way as R-1: names and layouts can change until Renga's GPU stage
fixes them, and any change bumps the version field. New tables are reached
through `get_accelerant_hook()` with feature codes in the same `0x5247xxxx`
range as R-1 and F-59 (`B_GPU_PRESENT_HOOKS` 0x52474100,
`B_GET_CURSOR_MAX_SIZE` 0x52474101).

What Quasar does today, for reference: one display (the first connected
one, on head 0), modes from NVKMS's validated list, EDID, DPMS on and off,
hardware cursor, and the R-1 table. NVKMS itself supports four heads,
hot-plug events, VRR, gamma LUTs and overlay layers; what is missing is the
interface to app_server and app_server's side.

### R-6 Several displays and hot-plug (P1)

**Driver side (Quasar):** one accelerant drives every connector of the GPU.
This replaces the single-display mode hooks for Renga's app_server; nothing
here needs to stay compatible with stock Haiku's. A new table describes
connectors and sets all displays at once:

```c
#define B_GET_DISPLAY_HOOKS	0x52474102
#define B_DISPLAY_HOOKS_VERSION	1

typedef enum {
	B_CONNECTOR_UNKNOWN, B_CONNECTOR_VGA, B_CONNECTOR_DVI,
	B_CONNECTOR_HDMI, B_CONNECTOR_DISPLAYPORT, B_CONNECTOR_EDP,
	B_CONNECTOR_USB_C
} display_connector_type;

typedef struct {
	uint32		id;				/* stable while the GPU is present */
	display_connector_type type;
	char		name[16];		/* "DP-1", "HDMI-1": for settings and UI */
	bool		connected;
	uint32		flags;			/* B_DISPLAY_CAN_VRR, B_DISPLAY_INTERNAL */
	uint32		vrr_min_hz;		/* 0 when no VRR */
	uint32		vrr_max_hz;
	uint32		gamma_size;		/* LUT entries per channel, 0 if none */
	uint32		max_bpc;		/* bits per colour the link allows */
} display_connector_info;

typedef struct {
	uint32		connector;		/* display_connector_info.id */
	bool		enabled;
	display_mode mode;			/* from that connector's mode list */
	int32		x, y;			/* position in the desktop, for the driver's
								   cursor across displays */
	uint32		flags;			/* B_DISPLAY_ALLOW_VRR */
} display_config;

typedef struct {
	uint32		version;		/* B_DISPLAY_HOOKS_VERSION */
	uint32		(*connector_count)(void);
	status_t	(*get_connector)(uint32 index, display_connector_info* info);
	/* Whole EDID with extension blocks (CTA-861, DisplayID), which carry
	   VRR and HDR capabilities that edid1_info leaves out. *size in: the
	   buffer's size; out: the EDID's. */
	status_t	(*get_edid)(uint32 connector, void* buffer, size_t* size);
	uint32		(*mode_count)(uint32 connector);
	status_t	(*get_mode_list)(uint32 connector, display_mode* modes);
	/* All displays in one call, applied together or not at all (NVKMS
	   validates a whole configuration, and a second display can make a
	   mode on the first impossible). B_BAD_VALUE leaves everything as it
	   was. */
	status_t	(*set_configuration)(const display_config* configs,
					uint32 count);
	status_t	(*get_frame_buffer_config)(uint32 connector,
					frame_buffer_config* config);
	status_t	(*set_dpms_mode)(uint32 connector, uint32 mode);	/* R-10 */
	status_t	(*set_gamma)(uint32 connector, const uint16* red,
					const uint16* green, const uint16* blue,
					uint32 size);								/* R-11 */
} display_hooks;
```

Hot-plug is an event on R-1's event descriptor, so app_server waits on one
descriptor for everything: `B_GPU_EVENT_HOTPLUG` with the connector id in
`gpu_present_event.head`, sent when a monitor is connected, removed, or its
EDID changes. That adds a value to `gpu_present_event_type` (and bumps
`B_GPU_PRESENT_VERSION` to 2, or reuse version 1 while it is provisional,
as Renga prefers).

**Renga side:** app_server with one `Screen` per enabled display in one
desktop coordinate space; `BScreen` for each (`B_MAIN_SCREEN_ID` stays the
primary); Screen preferences to arrange displays, choose the primary and set
each one's mode; settings kept per monitor (EDID manufacturer, product and
serial), so a monitor gets its mode back when it returns. On hot-plug:
re-read connectors, apply the saved or preferred configuration, and move
windows off a display that went away. The framebuffers stay
driver-allocated, as today (`get_frame_buffer_config` per connector).

### R-7 Variable refresh rate (P1)

**Driver side:** `display_connector_info` reports the VRR range (from
NVKMS, which reads the EDID and checks the link), `set_configuration` with
`B_DISPLAY_ALLOW_VRR` enables it for the mode (NVKMS needs it at mode-set
time), and R-1's `B_GPU_FLIP_VRR` asks for it on a flip.

**Renga side:** a per-display "Variable refresh rate" switch in Screen
preferences, on by default where supported; app_server sets
`B_DISPLAY_ALLOW_VRR` for it and uses `B_GPU_FLIP_VRR` only for a window
that presents directly (full screen, through the GPU stage), typically in
game mode (R-4). The desktop itself flips at a fixed rate, since VRR on
desktop updates makes the refresh rate swing visibly.

### R-8 Resizable BAR (P1)

The RTX 5070 can expose all 12 GB of video memory through BAR1. Without
Resizable BAR the BAR is 256 MB, and that is all the CPU can map: NVK's
host-visible video memory (uploads, persistently mapped buffers, which
games and emulators such as Eglantine use heavily) has to fit in it or fall
back to system memory.

**Renga side:** in the PCI bus manager, during resource assignment and
before drivers attach: find the Resizable BAR extended capability
(PCIe capability ID 0x15), choose the largest size the bridge window can
hold (moving a 64-bit prefetchable BAR above 4 GB when the firmware allows,
"Above 4G decoding"), program it, then assign as usual. A kernel setting
to turn it off, and the result in syslog. No driver interface: Quasar reads
the BAR size from `pci_info` as today (64-bit sizes).

### R-9 Shutdown and suspend/resume (shutdown P1, suspend P2)

**Shutdown (P1).** The driver needs to stop the GPU before a reboot or
power-off. NVIDIA's Linux driver has a PCI shutdown hook
(`nv_pci_shutdown()`) that marks the device as shut down and turns off bus
mastering, so the GPU stops DMA into memory that the firmware and the next
boot reuse. Haiku has no such hook, so the GPU keeps running until the
reset. Request: the kernel calls a driver hook at shutdown and reboot,
after app_server has quit and before the reboot itself:

```c
/* In device_hooks or the driver's module info: called once, in reverse
   probe order, with interrupts still working. May take up to 5 s. */
status_t (*shutdown)(void* cookie, bool reboot);
```

**Suspend and resume (P2).** For S3 sleep: `suspend(cookie, state)` and
`resume(cookie)` hooks, called children before parents on suspend and the
reverse on resume, plus a message to running applications before suspend
and after resume (`B_SYSTEM_SUSPENDING`, `B_SYSTEM_RESUMED`), so app_server
restores the display configuration and Vulkan applications recreate
swapchains. NVIDIA's suspend copies video memory in use to system memory,
so the driver needs to allocate up to that much during `suspend`; the
kernel must not have frozen allocation by then.

### R-10 Display power (P2)

The DPMS hooks exist and Quasar implements on and off. **Renga side:**
app_server turns displays off after the idle time set in the Screen Saver
or a power preference, and on again at the next input; per display through
`display_hooks.set_dpms_mode` once R-6 lands.

### R-11 Gamma and colour LUTs (P2)

For a night-light mode, monitor calibration, and games that set the gamma
ramp (SDL). **Driver side:** `display_hooks.set_gamma`, 16-bit entries,
`gamma_size` per channel (NVKMS takes 1024), applied at the next vblank.
**Renga side:** a private `BScreen` call to set and get the ramp, used by
the Screen preferences and SDL's Haiku back end; app_server restores the
saved ramp after a mode change or hot-plug.

### R-12 Overlay planes in app_server (P2)

The interface is R-1 (planes, `B_GPU_PRESENT_SCALING` and
`B_GPU_PRESENT_PLACEMENT`). **Renga side:** app_server puts a full-screen
window's buffer straight on the primary plane (direct scanout), and a video
window's frames on an overlay plane, with the desktop composited around it.
This is what makes video playback and windowed games skip the composition
copy.

### R-13 GPU monitoring (P2)

For the Graphics settings app Quasar ships, and for ActivityMonitor.
**Renga side:** a common interface every GPU driver can implement, so
system tools don't need to know NVIDIA's:

```c
/* <drivers/GpuMonitor.h>: an ioctl on the driver's device file. */
#define B_GPU_GET_STATS		0x52474110

typedef struct {
	uint32	version;				/* 1 */
	char	name[64];				/* "NVIDIA GeForce RTX 5070" */
	uint64	vram_total, vram_used;	/* bytes */
	uint32	gpu_load;				/* percent, 0-100 */
	uint32	memory_load;			/* percent */
	uint32	core_clock_mhz, memory_clock_mhz;
	int32	temperature_mc;			/* milli-degrees C, INT32_MIN if unknown */
	uint32	fan_rpm, fan_percent;	/* 0 if no fan reading */
	uint32	power_mw;				/* 0 if unknown */
} gpu_stats;
```

and ActivityMonitor data sources for GPU load, VRAM use and temperature.
Quasar implements it from RM's thermal, clock and utilisation controls.

### R-14 Hardware video decode in the Media Kit (P2)

Accepted on 2026-11-13 for FFmpeg (6.1.6 with `--enable-vulkan`). What is
left for Renga: that FFmpeg's plug-in tries Vulkan Video decode first when
a Vulkan device offers `VK_KHR_video_decode_queue` (through the
`VideoAcceleration.h` roster, or directly through FFmpeg's Vulkan hwaccel,
whichever Renga prefers), with software decode as the fallback; and that
decoded frames can reach the screen as R-1 buffers (dma-buf style
descriptors of the Vulkan images), so R-12's overlay plane shows them
without a copy. Quasar provides the Vulkan side (NVK in Mesa 26.3, H.264;
`docs/VIDEO.md`).

### R-15 Brightness, HDR and 10-bit (P3)

- **Brightness of external monitors** through DDC/CI (MCCS VCP code 0x10)
  over the display's I2C bus, which RM provides. The Screen preferences
  already use `B_SET_BRIGHTNESS`/`B_GET_BRIGHTNESS` for laptop panels;
  request: show the slider whenever the hook succeeds, and per display
  once R-6 lands.
- **10-bit output and HDR**: app_server is 8 bits per colour. When Renga
  takes this on, the driver side is a colour format in `display_config`
  (`B_RGBA1010102`) and HDR static metadata (CTA-861.3) per display; no
  work needed before then.

Not needed from Renga: power and clock management while running (GSP does
it), multi-GPU, and video encode (no NVENC; `docs/VIDEO.md`).

## Upstream header bug: `user_fd_kernel_ioctl()`

Not a request, a note for whoever touches it. `headers/private/kernel/fs/fd.h`
declares `user_fd_kernel_ioctl(int, ulong, void*, size_t)` inside
`extern "C"`, but `src/system/kernel/fs/fd.cpp` defines it in C++ with
`uint32 op`, which is a different, C++-mangled symbol. Code that uses the
header links against nothing. Quasar declares the C++ form itself
(`BaseDevice.cpp`, `KmsDevice.cpp`). If the definition is fixed to match the
header (C linkage), Quasar's two declarations must change in the same step,
or Quasar stops linking. The same mismatch is likely in stock Haiku.
