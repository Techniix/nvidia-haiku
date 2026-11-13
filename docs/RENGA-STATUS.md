# Quasar on Renga: status

Branch `renga`, created from `rtx5070-event-sync` at `13cae8c`. This branch
targets Renga only; the other branches remain the stock Haiku branches.

Each area has one of these states:

- **Tested**: run on the GeForce RTX 5070 (`10DE:2F04`, GB205), with the date
  and the Quasar revision.
- **Built**: compiles for Renga, not run on the card.
- **Written**: code exists, not built for Renga.
- **Blocked**: with what it waits on.
- **Not started**.

Nothing on this branch has been built for Renga or run on the RTX 5070 yet.
The "Tested" entries below are inherited results on stock Haiku hrev60368,
recorded in `docs/event-sync.md`.

"Syntax-checked" below means `tools/crosscheck/run.sh`: clang compiles the
kernel driver, the SDK wrappers and the accelerant with `-fsyntax-only`
against Renga's kernel headers and NVIDIA's headers. For the Mesa tree,
`tools/crosscheck/mesa.sh` does the same for NVK's RM back end and the Renga
WSI, with Mesa's generated headers made by Mesa's own generators. It finds
wrong types and signatures; it does not find link errors or GCC-only errors,
and says nothing about behaviour.

## Decisions

- **Native driver, no LinuxKPI (2026-11-11).** NVIDIA's kernel code is ported
  by supplying its OS interface, and `nvidia_gsp/nvidia/os-haiku*.cpp` does
  that directly on the Haiku/Renga kernel. That interface changes little
  between NVIDIA releases (570.86.16 and 570.133.07 have the same prototypes),
  so running NVIDIA's Linux glue on LinuxKPI would save little on updates,
  while LinuxKPI's DMA path bounces through a per-device pool and its coverage
  follows one Wi-Fi driver. Quasar stays native; where the kernel lacks
  something, it is requested in `RENGA-REQUESTS.md`.
- **Mesa on a branch, not patch files (2026-11-11).** `mesa-nvk` is the
  `renga` branch of `Techniix/mesa`: X547's `nvrm-event-sync-r4` at `643b3df`
  with the former `tools/patches/mesa-nvk` series as commits.

## Changed on this branch

| Change | State | Notes |
| --- | --- | --- |
| RM assertion failures no longer panic the kernel (`os_dump_stack`) | Syntax-checked | Was a `panic()` stub reached from `osAssertFailed()` on every failed `NV_ASSERT` |
| `os_dma_buf_enabled`, `nvidia_caps_root`, `nv_parms` are data, as RM expects | Syntax-checked | Were function stubs, so RM read machine code as their values |
| 68 OS-interface stubs implemented, made no-ops as on Linux x86, or turned into logged refusals | Syntax-checked | `os-haiku-misc.cpp`; 130 panic stubs remain, 111 of them NVLink/NVSwitch/IBM NPU/SPDM |
| RM log level: warnings and errors by default, `verbose true` in the driver settings file for everything | Syntax-checked | `nv_printf()` printed every level before |
| `registry_dwords` driver setting (RM registry values without a rebuild) | Syntax-checked | |
| NVK: routine notices only with `NVK_NVRM_DEBUG` | Host tests pass | `RM reports SM version`, `deferring reuse of a sync slot` |
| `tools/renga-test.sh`: build, install, run tests, collect one report archive | Smoke-tested on Linux | Timeout and collection paths only |
| GPU presentation interface, driver half (R-1) | Syntax-checked | Follows Renga's `<add-ons/graphics/GpuPresent.h>` (`744fa9b4`); `wait_fd` and repeating vblank events implemented; events on one eventfd; see Display |
| `nvkms_get_per_open_data()` (second-descriptor NVKMS ioctls) | Syntax-checked | Was `return NULL` |
| NVK: fence the USERD `GPPut` write and the doorbell; ring stalled channels again and report it | Syntax-checked | Candidate for the OBS hang |
| RM caching type `UNCACHED_WEAK` maps uncached, not write-back | Syntax-checked | `to_haiku_cache_type()` fell through to 0 for it; unknown types now map uncached with a log line |

## Inherited from `rtx5070-event-sync`

| Area | State | Notes |
| --- | --- | --- |
| Boot, GSP firmware 570.133.07, modesetting | Tested on stock Haiku | Not yet run on Renga |
| Vulkan through NVK (Mesa 26.2.2, X547 `nvrm-event-sync-r4` + patches 0001-0007) | Tested on stock Haiku | OpenCE through Zink runs |
| NVK patch 0008 (sync slot reuse, reset baseline) | Written | Host tests pass; not built for Haiku |
| NVK patch 0009 (per-channel stall report) | Written | Host tests pass; not built for Haiku |
| mesa-zink 0001 (pbuffers without a window) | Written | Not built for Haiku |
| OBS Studio through Zink | Hangs | Lost-doorbell fix built, not tested. See `docs/event-sync.md`, Open issues |
| Presentation (VideoStreamsWsi, three rotating bitmaps) | Tested on stock Haiku | Two copies and two waits per frame; rare tearing. `GetFormats` fix (whole `VkSurfaceFormatKHR` entries) applies; not built |

## Display

| Area | State | Notes |
| --- | --- | --- |
| Mode list, mode setting, EDID | Tested on stock Haiku | High refresh rates and all reported modes not verified |
| DPMS on/off | Written | Present in the accelerant; not verified |
| Hardware cursor | Host-tested | From `rtx5070-hw-cursor`; drag images up to 256×256 and Renga's `B_GET_CURSOR_MAX_SIZE` (`aa1cd0da`); fake-NVKMS test passes; not yet run on the RTX 5070 with Renga's drag change |
| Page flips with completion events (R-1 driver half) | Syntax-checked | `accelerant/NvPresent.cpp`, `headers/GpuPresent.h`: buffer import from an NVK memory descriptor (linear and block-linear), flips on any layer with position and scaling, completion through NVKMS notifiers and the modeset event queue, scanout export. Not run; app_server does not call it yet |
| Vblank events | Syntax-checked | `nvkms_get_per_open_data()` implemented (KmsDevice.cpp), so `NVKMS_IOCTL_NOTIFY_VBLANK` and the surface-grant and swap-group ioctls can resolve their second descriptor. `request_vblank` opens a one-shot descriptor per request; a helper thread merges vblank and flip events into one eventfd |
| Multihead, DisplayPort MST | Not started | app_server side is Renga F-38 |
| Variable refresh rate | Not started | |
| HDR | Not started | Stretch goal |
| HDMI/DisplayPort audio | Not started | NVIDIA HDA function; Renga's `hda` has HDMI/DP playback, untested on hardware |

## 3D and compute

| Area | State | Notes |
| --- | --- | --- |
| Renga WSI | Layer host-tested, Mesa platform syntax-checked (`mesa.sh`) | `docs/RENGA-WSI.md`. Default on this branch; `QUASAR_WSI=videostreams` falls back to VideoStreamsWsi. No CPU wait in the application's thread, no shared queue, acquire signalled by the driver. Zero-copy waits on F-47/F-58 |
| `VK_EXT_external_memory_host` | Blocked | Needs long-term pinning of user pages (`RENGA-REQUESTS.md`, R-5); `os_lock_user_pages()` refuses cleanly and NVK does not offer the extension |
| External memory/semaphore sharing between processes | Written (inherited) | NVK shares through RM's export-object-to-fd on an `nvidiactl` descriptor, not dma-buf; needs a test with two processes passing the descriptor |
| `VK_EXT_memory_budget` | Not started | |
| Extension coverage for Eglantine | Not started | Needs `vulkaninfo` from the card |
| Futex-based waits in Mesa | Blocked | Renga F-49 |
| OpenCL (rusticl) | Not started | |
| Pipeline/shader disk cache | Not started | |

## Video

| Area | State | Notes |
| --- | --- | --- |
| Vulkan Video in NVK | Not started | Investigation comes first |
| Media Kit hardware video back end | Not started | Interface exists on Renga: `headers/private/media/VideoAcceleration.h` (Media Kit phase 5, `446221a6`) |

## Power and system

| Area | State | Notes |
| --- | --- | --- |
| Clock and power management through GSP | Not started | The OS interface now reports "no runtime power management" as Linux without CONFIG_PM does; the idle state machine's mmap-revocation hooks are still panic stubs on purpose |
| Temperature, fan, clocks, utilisation, VRAM reporting | Not started | |
| Suspend/resume hooks | Not started | Renga F-37 |
| Recovery from channel and GPU faults | Not started | NVK 0009 turns an RC error into `VK_ERROR_DEVICE_LOST` during CPU waits |
| Quiet logging by default | Syntax-checked (kernel), host-tested (NVK), patch applies (VideoStreamsWsi) | `verbose true` in the driver settings file, `NVK_NVRM_DEBUG`, `VIDEOSTREAMSWSI_DEBUG` |
| Remaining panic stubs (`nvidia_gsp/nvidia/stubs.c`) | Reduced | 19 outside NVLink/NVSwitch/IBM NPU/SPDM: scatter-gather and peer DMA, page refcounts, IMEX channels, mmap revocation, PCI recovery |

## Renga integration

| Area | State | Notes |
| --- | --- | --- |
| Build and run unchanged on Renga | Not started | First hardware step |
| Mesa on a `renga` branch of `Techniix/mesa` (replacing `tools/patches/mesa-nvk`) | Done | Host tests pass against it; not built for Renga |
| `eventfd`/`memfd_create`/`epoll` paths in NVK and the SDKs | Not started | |
| Game mode hook | Blocked | Renga F-50 |
| Graphics settings panel | Not started | |
| RengaPorts recipes | Not started | |
