# Hardware cursor

The accelerant (`accelerant/Accelerant.cpp`) hands the mouse pointer to the display engine through NVKMS, so app_server no longer draws it into the framebuffer. It exports `B_SET_CURSOR_BITMAP`, `B_MOVE_CURSOR` and `B_SHOW_CURSOR`. `B_SET_CURSOR_SHAPE` (monochrome cursors) is still not implemented. app_server only uses it when `B_SET_CURSOR_BITMAP` refuses a cursor, and in that case it falls back to its software cursor anyway.

X547's accelerant already had this code, but behind `#if 0`. Simply switching it back on is not enough, as the test below shows; these are the changes.

## What changed

| Problem in the original code | Effect | Fix |
| --- | --- | --- |
| 32×32 cursors used a 32×32 surface whose rows `NvKmsBitmap` pads to 256 bytes. The cursor engine has no pitch register (NVKMS only programs a base address: `SetCursorSurfaceAddress()` in `nvkms-evo4.c` for the RTX 50 series, and in `nvkms-evo3.c` for earlier GPUs), so it expects packed rows. | Cursors of 32 pixels or smaller would come out scrambled. Most Haiku cursors are that size. | The smallest cursor surface is 64×64, where 256-byte rows are packed. The code checks that the rows are packed and refuses the cursor otherwise. |
| A modeset clears the cursor (`nvClearFlipEvoHwState()` in `nvkms-modeset.c`), and the accelerant never put it back. | After a resolution change the pointer disappeared until its shape next changed. | `SetDisplayMode` carries the cursor image and position in the `SET_MODE` request itself. If NVKMS refuses that, it sets the mode without the cursor and restores the cursor separately, so the cursor can never cost the mode. |
| A new image was only sent to NVKMS once the cursor was shown, and the result of that call was not reported. | A cursor NVKMS refused was reported to app_server as accepted, leaving an invisible pointer with no software fallback. | Only cursors that meet all of NVKMS's cursor rules are accepted (`B_RGBA32`, at most 256 and the device's `maxCursorSize`). A visible cursor's image is switched before the call returns, so a refusal reaches app_server, which then falls back to its software cursor. `show_cursor` and the cursor restore after a modeset can't report errors, so if NVKMS refuses the image there (for example while the head is off), the accelerant remembers that and retries from `move_cursor`: on the next move, then once per 64. The pointer comes back by itself instead of staying gone. |
| A failed image change still updated the hot spot and kept the new image pending, and `show_cursor(true)` did not resend the position. | The cursor showed in the wrong place after its hot spot changed while it was hidden, or after a failure. | Changes only take effect once NVKMS has accepted them, and showing the cursor also sends its position. |
| Replacing an image freed the old surface straight after submitting the new one. | Unregistering a surface makes NVKMS wait for the display channels to go idle (`nvEvoClearSurfaceUsage()` → `nvRMSyncEvoChannel()`), which right after a cursor update means waiting for it to reach the screen (up to a frame), with app_server's drawing lock held. | The replaced image is kept until the next change and freed *before* the next image is submitted, when the channels are long idle. So there are at most two cursor surfaces (16 KB each at 64×64). |
| Errors from moves and shows were logged on every call. | Syslog spam on every mouse move if something goes wrong. | The first error is logged, then one line per 1024. Every hook now also catches non-`system_error` exceptions, so none can unwind into app_server. |

Blending is `PREMULT_ALPHA`, matching what app_server's software cursor does when nothing is being dragged: `HWInterface::_DrawCursor()` blends the cursor bitmap as premultiplied, and so does `DrawingEngine::ReadBitmap()`. The bits are copied unchanged. One difference: the software blend adds in 8 bits and can wrap around where the hardware clamps, but only for pixels whose colour exceeds their alpha, which a premultiplied bitmap shouldn't have.

### Fixes in `nvrm_cpp_sdk` found on the way

- **Area leak.** `NvRmMemoryMapping`'s move assignment did not take over the area id. Every `NvKmsBitmap` that was move-assigned, which means every cursor change and every framebuffer change on a modeset, left its area mapped in app_server and later called `delete_area()` with an id that was already gone. At 1920×1080 that is about 8 MB of address space per resolution change.
- **RM mapping leak.** `NvRmDevice::MapMemory()` left the RM mapping behind when `NV_HAIKU_MAP` failed (this was marked as a TODO). It now unmaps it and reports the original error.
- **Throwing destructors.** `NvRmObject`, `NvKmsSurface` and `NvRmMemoryMapping` freed their resource in the destructor and in move assignment with calls that throw on failure. A throw out of a destructor terminates the process, and here that process is app_server. They now ignore errors while freeing (as the kernel would on process exit anyway), and move assignment checks for self-assignment.
- **Dangling error category.** `RaiseNvStatus()` threw `std::system_error` with a temporary `NvStatusErrorCategory`. `std::system_error` keeps a reference to its category, so `what()` or `code().category()` read freed stack memory, and every hook does that on an RM error. It now uses the static instance.

## What to expect

- **Drag bitmaps.** While a hardware cursor is in use, app_server does not draw drag bitmaps; `AccelerantHWInterface::_DrawCursor()` skips all cursor drawing then. Dragging an icon in Tracker shows only the pointer. This is an app_server limitation that applies to any driver with a colour hardware cursor; none of Haiku's own drivers export `B_SET_CURSOR_BITMAP`. Fixing it means changing app_server, for example to use the software cursor while a drag bitmap is shown. Note that Haiku does not accept AI-generated or AI-assisted contributions (its `AGENTS.md`), so a change like that would have to be written by a person to go upstream.
- **Screenshots and capture.** The cursor is no longer in the framebuffer. `BScreen::GetBitmap(..., drawCursor = true)` still includes it, because app_server composites its own copy (`DrawingEngine::ReadBitmap()`). Anything that reads the framebuffer directly, or asks for no cursor, won't see the pointer and has to draw it itself.
- **Hidden cursors reappear on shape changes.** After every successful `set_cursor_bitmap`, app_server calls `show_cursor(true)`, even if the cursor is meant to be hidden (`AccelerantHWInterface::SetCursor()`: `fAccShowCursor(cursorSet)`). The software cursor stays hidden in that case. The accelerant can't tell; the fix belongs in app_server (`fAccShowCursor(cursorSet && fCursorVisible)` or similar).
- **Cost and latency.** Each shape change allocates a small surface in video memory (16 KB for 64×64) and frees the one from the change before. A move is one NVKMS call: `nvMoveCursor()` → `MoveCursorC3()` writes the cursor's PIO registers. There is no RM/GSP call and no vblank or fence wait. It only polls the PIO `Free` count, which is non-zero unless the display front end is busy with a previous cursor update, and gives up after 0.1 s. NVKMS's own lock is shared only with app_server's other display calls (already serialised) and NVKMS's in-kernel timers. The cursor path uses no event-sync, NVK or semaphore code.

To measure the latency on a machine, add `cursor_timing true` to the settings file below. The syslog then gets lines like `nvidia_gsp: move_cursor: 4096 calls, average N us, max N us` (and the same for `show_cursor` and `set_cursor_bitmap`, per 64 calls).

## Turning it off

Create `~/config/settings/kernel/drivers/nvidia_gsp` containing:

```
hardware_cursor false
```

Then restart app_server (log out, or reboot). The same file takes `cursor_timing true` (see above). The accelerant reads the settings when app_server loads it, and with it off it doesn't export the cursor hooks, so app_server uses its software cursor as before. The syslog shows which mode is in use: `nvidia_gsp: hardware cursor enabled` or `... disabled by settings`.

## Test

`tests/accelerant-cursor/run.sh` builds the real `Accelerant.cpp` and `nvrm_cpp_sdk` sources against a fake RM and NVKMS. The fake wraps `open`, `close`, `ioctl`, `delete_area` and the driver-settings calls, and keeps account of every RM object, mapping, area, surface and device fd. It enforces the display engine's cursor rules: A8R8G8B8, pitch layout, square 32/64/128/256, packed rows. It also clears the cursor on a modeset that doesn't carry it, and takes surfaces off screen when they are unregistered.

The test drives the accelerant through `get_accelerant_hook()` as app_server does:
- 400 rounds of random shape changes from 1×1 to 256×256 with random hot spots, moves, and hiding and showing (including shape changes and moves while hidden);
- cursors it must refuse;
- a failure injected at each kernel call involved in making a cursor;
- modesets with the cursor shown, hidden, and refused by the driver;
- `show_cursor` and the post-modeset restore refused, once and lastingly (the cursor must come back, and the retries must stay rare);
- 20 modesets in a row;
- in child processes: the settings opt-out, timing on, and failing cleanup (unregister and RM free failing must not terminate the process).

The fake also flags any surface freed while a cursor update from the same call is still pending, which on hardware is the frame-long wait described above.

After each step it checks that what NVKMS would show (image, pixels and position) matches what app_server asked for, and that the resource counts haven't changed. At the end it requires that nothing is left. On non-Haiku hosts it builds with AddressSanitizer, UndefinedBehaviorSanitizer and LeakSanitizer. Run it on Haiku with no arguments. Elsewhere, set `HAIKU_HEADERS` to a Haiku tree's `headers/` and `NV_SRC` to an open-gpu-kernel-modules 570.133.07 tree.

With each fix reverted one at a time, the test fails:
- the area move;
- the 64-pixel minimum;
- the cursor in the modeset;
- the modeset retry;
- sending the position on show;
- freeing the old image before the update;
- the unmap on `NV_HAIKU_MAP` failure;
- the error category (ASan);
- quiet destructors;
- the stale-image retry;
- the opt-out;
- accepting `B_RGB32`.

The original code with only the `#if 0` removed fails it with leaked areas on every cursor change, unpacked 32-pixel cursors, missing cursors and wrong positions.

## Status

Built and tested on Linux only: `cursor-test` passes with sanitizers, and the changed files compile with gcc against Haiku's headers without new warnings. **Not yet run on Haiku or the RTX 5070.** What a fake can't show:
- whether the GB205 cursor channel accepts these surfaces;
- how the cursor looks;
- whether NVKMS really takes a cursor through `SET_MODE` without trouble;
- the real latency.

The fake follows the NVKMS source for all three. The first check on hardware is the syslog line, then moving the pointer, hovering over text and links (shape changes), changing resolution in Screen, and toggling the opt-out.
