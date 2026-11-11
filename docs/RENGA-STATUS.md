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

## Inherited from `rtx5070-event-sync`

| Area | State | Notes |
| --- | --- | --- |
| Boot, GSP firmware 570.133.07, modesetting | Tested on stock Haiku | Not yet run on Renga |
| Vulkan through NVK (Mesa 26.2.2, X547 `nvrm-event-sync-r4` + patches 0001-0007) | Tested on stock Haiku | OpenCE through Zink runs |
| NVK patch 0008 (sync slot reuse, reset baseline) | Written | Host tests pass; not built for Haiku |
| NVK patch 0009 (per-channel stall report) | Written | Host tests pass; not built for Haiku |
| mesa-zink 0001 (pbuffers without a window) | Written | Not built for Haiku |
| OBS Studio through Zink | Hangs | See `docs/event-sync.md`, Open issues |
| Presentation (VideoStreamsWsi, three rotating bitmaps) | Tested on stock Haiku | Two copies and two waits per frame; rare tearing |

## Display

| Area | State | Notes |
| --- | --- | --- |
| Mode list, mode setting, EDID | Tested on stock Haiku | High refresh rates and all reported modes not verified |
| DPMS on/off | Written | Present in the accelerant; not verified |
| Hardware cursor | Written | Hooks present in the accelerant; not verified on this branch |
| Page flip and vblank events | Not started | Needs the accelerant extension (`RENGA-REQUESTS.md`, R-1) |
| Multihead, DisplayPort MST | Not started | app_server side is Renga F-38 |
| Variable refresh rate | Not started | |
| HDR | Not started | Stretch goal |
| HDMI/DisplayPort audio | Not started | NVIDIA HDA function; Renga's `hda` has HDMI/DP playback, untested on hardware |

## 3D and compute

| Area | State | Notes |
| --- | --- | --- |
| Renga WSI (zero-copy) | Blocked | Accelerant extension, R-1 |
| `VK_EXT_external_memory_host` | Blocked | `os_lock_user_pages()` is a panic stub; NVK patch 0005 hides the extension |
| External memory/semaphore sharing between processes | Not started | dma-buf import paths are panic stubs |
| `VK_EXT_memory_budget` | Not started | |
| Extension coverage for Eglantine | Not started | Needs `vulkaninfo` from the card |
| Futex-based waits in Mesa | Blocked | Renga F-49 |
| OpenCL (rusticl) | Not started | |
| Pipeline/shader disk cache | Not started | |

## Video

| Area | State | Notes |
| --- | --- | --- |
| Vulkan Video in NVK | Not started | Investigation comes first |
| Media Kit hardware video back end | Not started | |

## Power and system

| Area | State | Notes |
| --- | --- | --- |
| Clock and power management through GSP | Not started | Power-management OS-interface paths are panic stubs |
| Temperature, fan, clocks, utilisation, VRAM reporting | Not started | |
| Suspend/resume hooks | Not started | Renga F-37 |
| Recovery from channel and GPU faults | Not started | NVK 0009 turns an RC error into `VK_ERROR_DEVICE_LOST` during CPU waits |
| Quiet logging by default | Not started | |
| Remaining panic stubs (`nvidia_gsp/nvidia/stubs.c`) | Open | About 80 OS-interface functions |

## Renga integration

| Area | State | Notes |
| --- | --- | --- |
| Build and run unchanged on Renga | Not started | First hardware step |
| Mesa on a `renga` branch of `Techniix/mesa` (replacing `tools/patches/mesa-nvk`) | Not started | |
| LinuxKPI helpers in `nvidia_gsp` | Not started | Audit first |
| `eventfd`/`memfd_create`/`epoll` paths in NVK and the SDKs | Not started | |
| Game mode hook | Blocked | Renga F-50 |
| Graphics settings panel | Not started | |
| RengaPorts recipes | Not started | |
