# Requests to Renga

Things Quasar needs from Renga (`Techniix/haiku`, `ce/main`). This file is
read by the Renga work to plan its side; Quasar's branch never changes that
repository. Each request states what is needed, the proposed interface and
the reason. A request is a proposal until the Renga side accepts it.

States: **Draft** (not ready to act on), **Proposed**, **Accepted**,
**Landed** (with the Renga commit).

| ID | Request | State | Roadmap entry |
| --- | --- | --- | --- |
| R-1 | Vendor-neutral accelerant extension: buffer import/export, page flip with completion events, vblank events | Draft | F-35, F-47 |
| R-2 | Scanout and window buffer export for privileged capture clients | Draft | F-47, F-31 |
| R-3 | `futex()` wait/wake | Already planned | F-49 |
| R-4 | Game mode notifications for drivers | Draft | F-50 |
| R-5 | Long-term pinning of user pages for device DMA | Proposed | F-35 (NVK coverage), F-48 |

## R-1 Accelerant extension

Draft. The interface will be written here once Quasar's driver half has been
prototyped against NVKMS, so the proposal reflects what the hardware path
can deliver (flip completion and vblank event sources, buffer handle type).
The requirements from F-47 are: GPU buffer import and export by driver
handle, page flips with completion events, vblank events, the same interface
for the future LinuxKPI DRM drivers.

## R-2 Buffer export for capture

Draft. A privileged client (Sunshine, OBS, the Media Kit capture API) obtains
the current scanout buffer or a window's buffer as a GPU handle. Depends on
the handle type chosen in R-1.

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
