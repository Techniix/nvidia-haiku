# Event-sync NVK on Haiku: notes for `rtx5070-event-sync`

This branch builds NVK from X547's `nvrm-event-sync-r4` Mesa branch (Mesa 26.2.2, pinned at `643b3df`) instead of the Mesa 25.3 NVK used on `rtx5070-dev`. This page records what was needed to make it work on Haiku with an RTX 5070, what each fix is for, and what is still open.

## Why event-sync

The older NVK RM backend waits for the GPU after every submission (`nvkmd_nvrm_exec_ctx_flush()` polls the channel semaphore, and `vk_sync` objects are no-ops). CPU and GPU never overlap, and a missed wake-up can cost up to a second.

`nvrm-event-sync-r4` submits without waiting. It implements `vk_sync` with real semaphore words and `NV01_EVENT` OS events, and waits only when an application asks to, or when a command-buffer slot is reused.

X547's published `nvidia-haiku` (May 2026) still uses `mesa-nvk-r2`. As far as this fork knows, `nvrm-event-sync-r4` had not been paired with a published Haiku kernel driver before. It expects a few things from the kernel side that were missing, listed below.

## Status

Tested on a GeForce RTX 5070 (`10DE:2F04`, GB205), Haiku hrev60368, November 2026, with the `quasar` package (revision 5):

| What | Result |
| --- | --- |
| Boot, desktop, modesetting | Works |
| Vulkan device | NVIDIA GeForce RTX 5070 (NVK GB205-A), SM 120 |
| OpenCE (Halo) through Zink | Runs and quits normally; no freezes |
| Frame tearing | Rare (a line now and then), much less than before the VideoStreamsWsi fix |
| Firefox through Zink | Starts and renders; long sessions not tested yet |
| OBS Studio through Zink | Hung in its first frames before `0008`; the fix is not yet tested on hardware |

## Changes in this branch

### Kernel driver (`nvidia_gsp`)

**Answer `NV_ESC_CHECK_VERSION_STR`** (`BaseDevice.cpp`). On Linux this ioctl is handled by the OS layer (`nvidia_ioctl`), not by `rm_ioctl()`. Event-sync NVK sends it first, to check that it was built for the running RM release, and fails device enumeration without an answer. The handler calls `rm_perform_version_check()`.

**Implement `os_string_copy`, `os_string_length`, `os_string_compare` and `os_strtoul`** (`os-haiku.cpp`, removed from `stubs.c`). These were still `panic()` stubs. RM's version check copies its version string with `os_string_copy()`, so the first NVK to ask for the version panicked the kernel. With the NVIDIA driver owning the display, the kernel debugger is not visible, so this looked like a hard freeze whenever any Vulkan or OpenGL application started. `os_strtoul` is written out so the driver does not depend on the kernel exporting `strtoul`. This fix is also on `rtx5070-dev`.

### NVK patches (`tools/patches/mesa-nvk/`)

`Build.sh` applies these in order on top of the pinned commit and skips patches that are already applied.

| Patch | What it does |
| --- | --- |
| `0001` | Derives the shader model from the chipset, as nouveau's `sm_for_chipset()` does. RM reports `0xa04` on GB205; the real SM is 120. Fixes scattered, stretched geometry. |
| `0002` | Keeps VA allocations in [8 GiB, 512 GiB), clear of NVK's shader local/shared memory windows. Fixes SM "Invalid Address Space" channel hangs (seen with occlusion queries). |
| `0003` | Builds the Wayland WSI without `CLOCK_MONOTONIC_RAW`, which Haiku lacks. Build fix only. |
| `0004` | `NVK_NVRM_TRACE=<file>`: logs every RM ioctl before and after the call, with `fsync()`, so the last call before a hard hang survives a power cycle. Off unless set. |
| `0005` | Does not offer `VK_EXT_external_memory_host` on Haiku. Host-pointer import goes through `ALLOC_OS_DESCRIPTOR`, which needs `os_lock_user_pages()`, still a panic stub in the kernel driver. |
| `0006` | Reports waits that stall: a CPU wait on a fence or semaphore, or a channel wait, prints its state to stderr after 3 s and then every 5 s (`nvk/nvrm: ...`). The wait itself is unchanged. |
| `0007` | Bounds GPFIFO reuse by the channel fence instead of USERD `GPGet`. See below. |
| `0008` | Does not reuse sync backing (a pool slot, or a dedicated sync's memory) while queued GPU work may still use it, and stops a reset binary semaphore from reading signaled again. Fixes the OBS Studio hang. See below. |

**The GPFIFO hang (`0007`).** Applications froze a few seconds into rendering, and Halo also froze on quit. The render thread was in `vkQueueSubmit -> nvk_queue_submit -> nvkmd_nvrm_exec_ctx_exec -> chan_write_gp_fifo_entry -> poll`, waiting for USERD `GPGet` to move away from the next `GPPut`. The slot-recycle wait, which reads the channel's fence semaphore, never stalled, so the GPU was completing work, but `GPGet` as read through the CPU mapping did not keep up. Once `GPPut` wrapped around the 0x8000-entry ring and met the stale value, the loop waited forever. The fix records `GPPut` at the end of each submit cycle next to its fence, and treats the `GPPut` of the newest completed cycle as the host's read position. This never frees an entry before the host has executed it. Why `GPGet` does not advance as seen by the CPU is not understood yet.

**The OBS hang (`0008`).** OBS Studio renders through Zink with one GL context shared between threads and never presents to a window. It hung within its first frames: its graphics thread waited forever on a timeline sync, and the stall report (`0006`) showed `timeline wait_value 16: value 15 submit_seq 16`. Submission 16 had been emitted, but its release never ran. Halo never hit it. From the code:

1. The nvrm sync type has no `VK_SYNC_FEATURE_WAIT_BEFORE_SIGNAL`, so Mesa's runtime uses `VK_DEVICE_TIMELINE_MODE_ASSISTED` and runs queues as `THREADED_ON_DEMAND` (`vk_device.c`, `get_timeline_mode()`). The first submission that waits on a timeline point not yet submitted switches its queue to a submit thread for good (`vk_queue.c`, the zero-timeout `VK_SYNC_WAIT_PENDING` check before `vk_queue_enable_submit_thread()`).
2. In that mode, `vk_queue_submit_move_binary_waits_to_temps()` moves every binary-semaphore wait into a temporary `vk_sync`, and `vk_queue_submit_cleanup()` destroys the temporary as soon as the driver has accepted the submission, before the GPU runs it.
3. Two NVK bugs then let the GPU acquire for that wait target a semaphore word that will never get the value it needs. The channel parks on the acquire with no RC error, and everything queued behind it never runs, including the timeline release the graphics thread waits for.
   - **Slot reuse.** NVK's pooled `nvkmd_nvrm_sync_move()` swaps slots, so the temporary owns the slot the queued acquire reads. Destroying the temporary returned the slot to the pool. The next allocation took the lowest free slot (that one) and zeroed it.
   - **Stale reset.** In immediate mode the runtime resets a binary semaphore right after submitting its wait, usually before the GPU has run the signal it consumes (`value` k-1, `submit_seq` k). `sync_reset_data()` set the baseline to `value`, so once the release landed the semaphore read as signaled, and pending, again. After the switch to a submit thread, a later wait whose signal was still queued in that thread passed the move's `WAIT_PENDING` on this stale state. The backings were swapped before the signal picked its address, so the release went to the new slot while the moved acquire waited on the old one.

Both are fixed, not timed out:
- **Deferred reuse.** A freed slot, or the last reference to a dedicated sync's memory (a temporarily imported payload, say), is kept until every channel has completed the fence by which it will have executed everything already emitted. That is the fence of its last submission, or the next one if its push still holds unsubmitted methods.
  - Exec and bind channels register when they finish init, and init fails if that can't be recorded. They unregister once their RM channel is freed.
  - `wSeq` is published before the push is reset, so a concurrent reader errs late, never early.
  - The logic is in `nvkmd_nvrm_retire.h`, free of driver types so it can be tested directly.
  - The first deferral prints `nvk/nvrm: deferring reuse of a sync slot until queued GPU work retires (...)` once per device. That line is the sign that a run took this path.
- **Reset baseline.** A reset now raises the baseline to cover submitted signals as well as completed ones.

To check on a machine whether a hang is in this area, run the app with `MESA_VK_ENABLE_SUBMIT_THREAD=0`, which keeps queues in immediate mode. If the hang goes away, it is a submit-thread-mode bug. `=1` starts every queue in that mode.

### Zink and the Haiku EGL driver (`tools/patches/mesa-zink/`)

**`0001`: pbuffers without a window.** A pbuffer's framebuffer has no `BitmapHook`, but `haiku_make_current()` called `update_size()`, which called `GetSize()` through that NULL pointer; OBS avoided pbuffers for this reason. The hgl frontend also created pbuffer color buffers as Zink window drawables, with `PIPE_BIND_DISPLAY_TARGET`. Now the size query is skipped for windowless buffers, `eglSwapBuffers()` on a pbuffer is a no-op as EGL 1.5 requires, and windowless buffers get plain render-target and depth textures. `Build.sh` now applies patches to mesa-zink too.

### Window-system layer (`tools/patches/VideoStreamsWsi/`)

**`0001`: rotate three present bitmaps.** `QueuePresent` copied every frame into one host-visible image and kept handing the same `BBitmap` to the view. app_server draws that bitmap asynchronously, so a frame could be written while it was being drawn, which showed as a horizontal tear. VSync cannot help, because this path has no flip. The layer now keeps three bitmaps and copies each frame into one that is not on screen. The bitmap on screen belongs to the view's `BitmapHook`, as before. `Build.sh` now applies patches to VideoStreamsWsi too.

### Packaging (`tools/`)

- `make-package.sh` finds NVK where Mesa 26 installs it on Haiku (`add-ons/vulkan/icd.d`), with its manifest. It prints the Mesa version it packages, because a stale Mesa 25.3 copy in `lib/` would otherwise be packaged silently.
- `install-package.sh` warns after installing if the Vulkan loader can see any other NVK (system, user or non-packaged folders) or a `VK_DRIVER_FILES` / `VK_ICD_FILENAMES` / `VK_ADD_DRIVER_FILES` setting.

## Building and installing

```sh
git checkout rtx5070-event-sync && git pull
git submodule sync && git submodule update --init
SKIP_NVIDIA_OBJECTS=1 ./Build.sh 2>&1 | tee ~/nvk-build.log
grep -n -m1 -A40 "^FAILED" ~/nvk-build.log   # should print nothing
tools/make-package.sh --revision <n>         # check: "NVK from add-ons/vulkan/icd.d/... (Mesa 26.2.2)"
tools/install-package.sh
```

Reboot after any change to the kernel driver. For NVK, Zink or VideoStreamsWsi changes alone, restarting the applications is enough. Keep the previous `.hpkg`: `tools/install-package.sh <older>.hpkg` rolls back. If Haiku does not reach the desktop, boot with **Disable user add-ons** and roll back from there.

## Regression tests (`tests/`)

`tests/build.sh run` builds and runs the GPU tests on the Haiku machine. `tests/sync-pool/run.sh` runs the host test (no GPU needed) against the patched `mesa-nvk` tree after `Build.sh`.

| Test | What it checks |
| --- | --- |
| `vk-binary-wait-reuse` | Both `0008` bugs, at the Vulkan level with no window. Part 1 runs immediate-mode signal/wait rounds on a binary semaphore, switches the queue to submit-thread mode, then waits on that semaphore while its signal is held back in the submit thread. Part 2 completes a signal, queues slow work and a wait on it, and allocates semaphores while the GPU is busy. It fails with a timeout if the queue stalls. `--no-thread` runs part 2 in immediate mode as a control. |
| `egl-surfaceless-threads` | OBS's GL pattern: a surfaceless 3.3 core context, an FBO with an RGBA8 renderbuffer, draw + `glReadPixels` every frame on one thread, while a second thread borrows the context for `glTexSubImage2D` + `glFlush`. Every readback is checked, and a watchdog fails it if no frame completes for 10 s. `--pbuffer` uses a pbuffer instead (mesa-zink `0001`), and `--fps 30` paces it like OBS. |
| `sync-pool/` | Compiles the real sync-pool functions and `sync_reset_data()` from the patched `nvkmd_nvrm_sync.c` against fake channels. It checks that a freed slot is never handed out before its retire point, that unsubmitted push contents count, that every channel must retire (including one that submits while the slot is being freed), that destroying a channel releases what waited on it, that dedicated memory is unreffed only after retirement, and that a reset leaves a binary sync unsignaled after its pending signal lands. It includes a randomised stress run. The free-at-once pool, the two-pass retire and the old reset each make it fail. |

## Debugging aids

- `nvk/nvrm: ... not satisfied` or `... waiting` on stderr (`0006`) names a stuck wait and its counters.
- `NVK_NVRM_TRACE=<file>` (`0004`) records every RM call. If the last line starts with `>`, the system hung inside that call.
- An application that hangs with no stall message: open `Debugger --team <id>`, select the busiest thread, click **Debug**, then save a report. Threads must be stopped to appear in the report. Debugger may fail to stop Zink's helper threads, whose names are garbled.
- `listimage <team> | grep nouveau` shows which NVK an application loaded. Run without a team ID, it lists every process.
- Test a build by installing it as a package rather than pointing `VK_DRIVER_FILES` at the build folder, so there is no doubt about which NVK an application loads.
- `grep -a` is needed to search `/var/log/syslog`, which can contain binary bytes.

## Open issues

- **USERD `GPGet` does not advance as seen by the CPU.** `0007` works around it; the cause (mapping, cache attributes, or Blackwell host behaviour) is unknown.
- **Remaining panic stubs.** About 80 OS-interface functions in `stubs.c` still `panic()`. Those checked are on paths NVK does not use (dma-buf import, NUMA, power management, user-page pinning), but a new code path can reach one, and that looks like a hard freeze.
- **Rare tearing** remains. If app_server falls more than a frame behind, a fourth present bitmap may help.
- **`GALLIUM_THREAD=0`** makes Halo crash during startup. Not investigated; normal runs use threading.
- **Firefox kill hang.** On the older driver, closing a frozen Firefox could hang the system in teardown. Not reproduced on this branch yet; long Firefox sessions still need testing.
- **OBS on hardware.** `0008` was found and fixed from the code, and checked with the host harness and compile checks only. Still to be done on the RTX 5070: `tests/build.sh run`, `tests/sync-pool/run.sh`, OBS (preview, recording, resize, projector) and OpenCE. A run that took the fixed path prints the one-time `nvk/nvrm: deferring reuse of a sync slot ...` line.
- **Upstream.** The kernel string functions, `0007` and the VideoStreamsWsi fix apply to X547's trees too and should be offered there.
