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
