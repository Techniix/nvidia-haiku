# nvidia-haiku

> **This is Techniix's fork of [X547/nvidia-haiku](https://github.com/X547/nvidia-haiku)**, used to bring up the GeForce RTX 5070 (Blackwell).
> Branches: `rtx5070-dev` (570.133.07, submodules point at this account's forks), `test-a-570.86.16` (X547's 570.86.16 plus the fixes), and `x547/rtx5070-v2` (the same changes as a clean series for upstream, submodule URLs unchanged; `x547/rtx5070` is an older ordering).
> Prebuilt test builds are on the [Releases](https://github.com/Techniix/nvidia-haiku/releases) page.
> NVK and NVIDIA-tree changes live in [Techniix/mesa](https://github.com/Techniix/mesa) (`rtx5070-dev`) and [Techniix/open-gpu-kernel-modules](https://github.com/Techniix/open-gpu-kernel-modules) (`570.133.07-haiku`).

A hardware-accelerated graphics driver for NVIDIA GPUs on [Haiku](https://www.haiku-os.org), built on NVIDIA's [open GPU kernel modules](https://github.com/NVIDIA/open-gpu-kernel-modules). It provides modesetting, Vulkan through Mesa's NVK, and OpenGL through Zink.

It targets GPUs that run NVIDIA's GSP firmware: Turing and newer.

## How it fits together

| Layer | Component | Role |
| --- | --- | --- |
| Kernel | `nvidia_gsp` | Haiku kernel driver. Hosts NVIDIA's Resource Manager (`nv-kernel.o`), which boots the GPU's GSP firmware and manages memory, channels and interrupts. Publishes `/dev/graphics/nvidia<N>` and `/dev/nvidiactl`. |
| Kernel | `nvidia_gsp_modeset` | Hosts NVIDIA's NVKMS (`nv-modeset-kernel.o`) for display. Binds only to an `nvidia_gsp` built from the same NVIDIA release. |
| User | `nvidia_gsp.accelerant` | app_server accelerant: modes and EDID through NVKMS. |
| User | Mesa NVK (`mesa-nvk`) | Vulkan driver, with a backend that talks to NVIDIA's RM instead of nouveau. |
| User | Mesa Zink (`mesa-zink`) | OpenGL on top of Vulkan. |
| User | `VideoStreamsWsi` | Vulkan window-system integration for Haiku. |
| SDK | `nvrm_sdk`, `nvrm_cpp_sdk` | NVIDIA headers and RM/NVKMS ioctl wrappers for the user-space components. |

The Haiku glue between NVIDIA's code and the Haiku kernel lives in `nvidia_gsp/nvidia/os-haiku.cpp`. Interfaces not implemented yet are listed in `nvidia_gsp/nvidia/stubs.c`, where they `panic()` if reached.

## Versions

The NVIDIA tree is the `open-gpu-kernel-modules` submodule. Its release number must match three other things:

- the GSP firmware in `firmware/<version>/`, which RM checks at boot;
- the driver's module name and firmware path, both taken from `NV_VERSION_STRING`;
- the user-space builds: RM rejects clients built against a different release, so rebuild NVK and Zink after changing it.

| NVIDIA release | Notes |
| --- | --- |
| 570.86.16 | Original base. Its device table lists GeForce RTX 5090, 5090 D and 5080 among Blackwell cards. |
| 570.133.07 | First release listing the GeForce RTX 5070 (`10DE:2F04`). Same OS-interface prototypes as 570.86.16. |

## Building

Build natively on Haiku x86_64 with `meson`, `ninja`, GCC and Haiku's private kernel headers under `/boot/system/develop/headers/private`. The Mesa builds also need libclc, SPIRV-LLVM-Translator and SPIRV-Tools (see the paths in `Build.sh`); libclc is not in HaikuPorts.

```sh
git clone --recursive <this repository>
cd nvidia-haiku
./Build.sh
```

`Build.sh` first builds NVIDIA's `nv-kernel.o` and `nv-modeset-kernel.o` with NVIDIA's Makefiles. It then links the NVIDIA tree where `nvrm_sdk` and NVK expect it, and builds each project with meson into `install.<arch>`. Projects may sit at the repository root or under `source/`; missing ones are skipped.

Options: `JOBS=<n>` sets parallelism, `NV_MAKE_ARGS` passes extra flags to NVIDIA's Makefiles, and `SKIP_NVIDIA_OBJECTS=1` reuses already-built objects.

After changing the NVIDIA release, delete `build.<arch>`, `install.<arch>` and the NVIDIA `_out` directories before rebuilding.

## Installing

After `Build.sh`, or from an unpacked release:

```sh
tools/install.sh --check   # shows what it would change; changes nothing
tools/install.sh           # installs, after asking
```

The installer:

- checks for Haiku x86_64, an NVIDIA GPU, the build, its firmware and checksums, and the libraries NVK links against. It offers to fetch missing libraries with `pkgman`;
- finds earlier copies of the driver in either `non-packaged` folder (a previous install, a test install, hand-copied files, old firmware) and moves them into `~/nvidia-haiku-backups/<date>`. Nothing is deleted except the files of its own previous install;
- installs the kernel driver, accelerant, NVK, the Vulkan window-system layer and the GSP firmware into `~/config/non-packaged`, recording each file in `~/config/settings/nvidia-haiku/manifest`;
- if a package also installs an NVK (for example `nebula`), sets `VK_DRIVER_FILES` in `~/config/settings/boot/UserSetupEnvironment`, so every application uses this build after a reboot.

It never modifies packages or `/boot/system/settings`. It stops if another package's `nvidia_gsp` kernel driver is active, since two copies would compete.

`tools/uninstall.sh` removes exactly what was installed, along with the Vulkan setting. `tools/uninstall.sh --restore` also puts back everything the installer moved aside, returning the system to how it was before the first install.

If Haiku doesn't reach the desktop, hold Space while booting and pick **Disable user add-ons** (or **Use fail-safe graphics driver**) in *Safe mode options*, then run `tools/uninstall.sh`.

The firmware search order is user non-packaged, user, system non-packaged, then system data directories. `tools/install-test.sh` is the older, minimal installer, kept for development.

## Debugging

- `grep -iE "nvidia_gsp|NVRM" /var/log/syslog` shows device detection, the firmware path loaded, and RM errors such as `RmInitAdapter failed!`.
- `[!!!] <name>: not implemented` in the log, or as a panic, names an OS-interface function from `stubs.c` that RM reached.
- `NVK_NVRM_DEBUG=1` makes NVK print the device description it uses: chipset, shader model, GPC/TPC counts, VRAM and BAR1 size.
- If an application renders garbage, check that it loads this NVK: `VK_LOADER_DEBUG=driver` prints the driver chosen. An older NVK installed system-wide (for example by the `nebula` package, in `/boot/system/add-ons/vulkan/icd.d`) can take precedence; set `VK_DRIVER_FILES` to this build's `nouveau_icd.x86_64.json` to use it alone.
- Boot-menu *Debug options* can save the previous session's syslog to `/var/log/previous_syslog`, or to a file on another volume when Haiku doesn't boot.

## Status

Experimental. Expect crashes, especially under heavy GPU load.

Tested on a GeForce RTX 5070 (`10DE:2F04`, GB205, Blackwell), November 2026:

| NVIDIA release | Boot and firmware | Vulkan device | Rendering (game, OpenGL via Zink) |
| --- | --- | --- | --- |
| 570.133.07 (`rtx5070-dev`) | Loads `570.133.07/gsp_ga10x.bin`, family gb20x | NVIDIA GeForce RTX 5070 (NVK GB205-A) | Correct |
| 570.86.16 (`test-a-570.86.16`) | Loads `570.86.16/gsp_ga10x.bin`, family gb20x | NVIDIA Graphics Device (NVK GB205-A) | Correct |

On GB205, RM reports shader model `0xa04`; NVK now takes SM 120 from the chip ID instead, which fixed the scattered, stretched geometry seen before. Other Turing-and-newer GPUs should work but are untested with these changes.
