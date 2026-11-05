# nvidia-haiku

> **This is Techniix's fork of [X547/nvidia-haiku](https://github.com/X547/nvidia-haiku)**, used to bring up the GeForce RTX 5070 (Blackwell).
> Branches: `rtx5070-dev` (570.133.07, submodules point at this account's forks), `test-a-570.86.16` (X547's 570.86.16 plus the fixes), and `x547/rtx5070` (the same changes as a clean series for upstream, submodule URLs unchanged).
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

Copy the runtime parts of `install.<arch>` into a `non-packaged` folder, for example `~/config/non-packaged`:

- `add-ons/kernel/drivers/bin/nvidia_gsp` and `nvidia_gsp_modeset`, with the `dev/` symlinks next to them
- `add-ons/accelerants/nvidia_gsp.accelerant`
- the Mesa libraries and Vulkan ICD files

Then copy the firmware to `data/firmware/nvidia/<version>/` in the same folder. The driver searches the user non-packaged, user, system non-packaged and system data directories, in that order. Reboot to load the driver.

To recover from a driver that won't start: hold Space while booting and pick **Disable user add-ons** or **Use fail-safe graphics driver** in *Safe mode options*.

## Debugging

- `grep -iE "nvidia_gsp|NVRM" /var/log/syslog` shows device detection, the firmware path loaded, and RM errors such as `RmInitAdapter failed!`.
- `[!!!] <name>: not implemented` in the log, or as a panic, names an OS-interface function from `stubs.c` that RM reached.
- `NVK_NVRM_DEBUG=1` makes NVK print the device description it uses: chipset, shader model, GPC/TPC counts, VRAM and BAR1 size.
- Boot-menu *Debug options* can save the previous session's syslog to `/var/log/previous_syslog`, or to a file on another volume when Haiku doesn't boot.

## Status

Experimental. Expect crashes, especially under heavy GPU load.

Support for the GeForce RTX 5070 (GB205, Blackwell) and other Blackwell cards was added in late 2026 and has compile-time verification only so far: it builds and links for Haiku against 570.86.16 and 570.133.07. Hardware results will be recorded here.
