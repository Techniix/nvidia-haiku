# app_server: drag bitmaps disappear with a hardware cursor, and hidden cursors reappear

**Report for the Haiku developers**

> **Disclosure.** The analysis and patches 0002 and 0003 were written by an AI assistant (Claude, by Anthropic).
> Dalton Singleton (GitHub: Techniix) asked for them while enabling a hardware cursor in his NVIDIA accelerant
> ([nvidia-haiku](https://github.com/Techniix/nvidia-haiku)). Haiku's contribution policy does not accept
> AI-generated code. The patches are offered only as a precise description of the problems and of one possible
> fix. Please rewrite them, adapt them or discard them as you see fit. They have been syntax-checked and reviewed,
> but not built with jam or run; see [Verification](#5-verification).

| | |
| --- | --- |
| Component | app_server: `drawing/interface/local/AccelerantHWInterface`, `drawing/HWInterface` |
| Affects | Every accelerant that exports `B_SET_CURSOR_BITMAP`, or `B_SET_CURSOR_SHAPE` for BeOS `BCursor` cursors |
| Symptoms | (1) Dragging with a drag bitmap (icons in Tracker, for example) shows only the pointer. (2) A hidden or obscured cursor reappears on the next cursor change. |
| Base revision | haiku master `b9fdc8a9e6` (2026-11-06), on top of patch 0001 |
| Patches | `0002-app_server-ReadBitmap-read-front-buffer-areas-with-t.patch` (3 files, +6/−27), `0003-app_server-show-drag-bitmaps-with-a-hardware-cursor.patch` (2 files, +105/−7) |

The three patches form one series:

| Patch | Author | What it does |
| --- | --- | --- |
| 0001 | OBS Studio port session | `BScreen::ReadBitmap()` reads from the back buffer, so screen recorders stop stalling the desktop and pointer. See `app_server-readbitmap-report.md`. |
| 0002 | this report | Corrects how 0001 handles the software cursor in areas read from the front buffer, and removes `HWInterface::SoftwareCursorFrame()`. Can be squashed into 0001. |
| 0003 | this report | Uses the software cursor while a drag bitmap is shown, and keeps hidden cursors hidden. Independent of 0001 and 0002 apart from context lines. |

## 1. Observed behavior

The test system has an NVIDIA GeForce RTX 5070 with the `nvidia_gsp` accelerant, which now exports
`B_SET_CURSOR_BITMAP`, `B_MOVE_CURSOR` and `B_SHOW_CURSOR`.

- Dragging an icon in Tracker shows only the pointer, not the icon. Drag-and-drop still works, but you can't see
  what you are dragging.
- No in-tree driver exports `B_SET_CURSOR_BITMAP`; vesa has it only behind `FAKE_HARDWARE_CURSOR_SUPPORT`. So this
  path is rarely exercised. Drivers that export `B_SET_CURSOR_SHAPE` (intel_extreme, ati, s3, 3dfx) get a hardware
  cursor only for monochrome `BCursor` cursors. Those drivers would hit the same two problems while one is in use.

## 2. Root cause

### Drag bitmap

- `HWInterface::_AdoptDragBitmap()` combines the cursor and the drag bitmap into `fCursorAndDragBitmap`, which
  only the *software* cursor draws.
- `AccelerantHWInterface::SetCursor()` gives the accelerant only the plain `ServerCursor`.
- `AccelerantHWInterface::_DrawCursor()` draws nothing while `fHardwareCursorEnabled` is set, and
  `HWInterface::_CursorFrame()` is empty then.

So once a hardware cursor is in use, the drag bitmap is never drawn anywhere. Nothing switches to the software
cursor for a drag: `SetDragBitmap()` isn't overridden, and the hardware/software decision is only made in
`SetCursor()`.

### Hidden cursor

`AccelerantHWInterface::SetCursor()` ends with `fAccShowCursor(cursorSet)`, so every successful cursor change shows
the hardware cursor. That includes the case where the cursor is hidden: `SetCursorVisible(false)`,
`BApplication::HideCursor()` through `ServerApp`, or `ObscureCursor()` while typing. The software cursor stays
hidden in the same situation, because `_CursorFrame()` is empty while `fCursorVisible` is false.

### 0002: the software cursor in 0001's front-buffer areas

0001 reads most of the screen from the back buffer. It reads from the front buffer only the areas whose current
content exists only there: direct windows, `BWindowScreen`, and windows in an update session. Inside those areas it
meant to read the software cursor's rectangle from the back buffer instead, using a new
`HWInterface::SoftwareCursorFrame()`. Two things are wrong with that:

- **There, the back buffer is not a valid source.** It is stale under direct windows and half redrawn (erased
  backgrounds) under windows in an update session. Reading the cursor's rectangle from it would put a stale or
  erased patch into the capture, the size of the cursor or, during a drag, of cursor plus drag bitmap.
- **The function never returned anything.** `SoftwareCursorFrame()` returned the `fCursorAreaBackup` rectangle,
  and `_AdoptDragBitmap()` only allocates that backup when the interface is **not** double buffered. 0001's path
  only runs when double buffered, so the exclusion never happened.

The back buffer never contains the software cursor anywhere: when double buffered, `_DrawCursor()` blends it from
the back buffer into the front buffer.

## 3. Proposed changes

### 0002

The front-buffer areas are read from the front buffer, software cursor included, as `ReadBitmap()` always did
before 0001. `SoftwareCursorFrame()` is removed. Outside those areas the back buffer is read, and it never
contains the software cursor.

What remains: where a *software* cursor is over a direct window or a window in an update session, a capture with
`drawCursor = false` contains it, and one with `drawCursor = true` contains it twice. That is the same as before
0001, and it is limited to those areas. With a hardware cursor it doesn't arise.

### 0003

All changes are in `AccelerantHWInterface`. `HWInterface` and the other interfaces are untouched.

- **`_UpdateHardwareCursor(cursor)`** is the hook part of the old `SetCursor()`, mostly unchanged. The differences:
  - It doesn't use the hardware while a drag bitmap is shown. `_DragBitmapShown()` applies the same tests as
    `_AdoptDragBitmap()`, including its colour spaces (`B_RGB32`, `B_RGBA32`): `_AdoptDragBitmap()` ignores drag
    bitmaps in other colour spaces, so the software cursor wouldn't show them and wouldn't follow cursor changes
    either.
  - It calls `show_cursor(cursorSet && IsCursorVisible())` instead of `show_cursor(cursorSet)`.
  - It sets `fHardwareCursorEnabled` while still holding the exclusive lock; the old code set it after unlocking.
  - It returns whether it switched from hardware to software.
- **`_DrawSoftwareCursor()`** invalidates `_CursorFrame()` after a hardware-to-software switch. When single
  buffered it calls `_DrawCursor()` directly, because `Invalidate()` does nothing there; normally unreachable,
  since `AccelerantHWInterface` always has a back buffer. `HWInterface` only
  draws the software cursor when the cursor or drag bitmap *changes*, so without this the cursor would not appear
  until the next move. The software-to-hardware direction was already handled: the existing code erases the
  software cursor.
- **`SetCursor()`** calls `_UpdateHardwareCursor()` and then `HWInterface::SetCursor()`, redraws the software cursor
  if needed, and only then releases the exclusive lock. The old code released it before `HWInterface::SetCursor()`.
  Holding it longer means `SetDragBitmap()` can't read the old `fCursor` and hand the hardware an outdated cursor in
  between. The lock is recursive (`rw_lock` allows the write holder to read- and write-lock again, and
  `ObscureCursor()` already relies on that).
- **`SetDragBitmap()` is overridden:**
  - It takes the exclusive lock and calls `HWInterface::SetDragBitmap()`. That function draws the cursor and drag
    bitmap into the frame buffer, and before this change it did so without the lock.
  - Still under the lock, it switches between hardware and software when a switch is due: a drag bitmap appeared
    while the hardware cursor is on, or it went away while it is off.
  - It does nothing more otherwise. `EventDispatcher` clears the drag bitmap after every drag, with or without a
    bitmap, so normally nothing needs switching. The exception is a cursor already in software because the
    hardware refused it: that cursor is offered to the hardware again.
  - Without cursor hooks it only adds the lock.

Lock order is unchanged. `EventDispatcher::SetDragMessage()` and `_DeliverDragMessage()` call `SetDragBitmap()`
with the EventDispatcher lock held. The override then takes the engine lock, then `fFloatingOverlaysLock`. The
EventDispatcher-then-engine order already exists: `MouseFilter` calls `Desktop::SetCursor()`, and the event loop
calls `MoveCursorTo()` when there is no cursor thread. No path takes the two locks the other way round.
`ServerWindow` releases the window lock before `SetDragMessage()`. The engine lock is recursive in both
`MultiLocker` builds: the release build uses `rw_lock`, which allows the write holder to lock again; the DEBUG build
counts nesting in `fWriterNest`.

### Walkthrough

| Event | Before | After 0003 |
| --- | --- | --- |
| Drag with a bitmap starts | Hardware cursor stays on. The drag bitmap is combined but never drawn. | `_AdoptDragBitmap()` combines as before. The override then hides the hardware cursor and draws the software cursor with the drag bitmap. |
| Cursor shape changes during the drag | Uploaded to the hardware; still no drag bitmap. | The software cursor is kept. `HWInterface::SetCursor()` recombines the new cursor with the drag bitmap. |
| Drop | Nothing to switch. | `_AdoptDragBitmap()` erases the drag bitmap as before. The override uploads the cursor, erases the software cursor and shows the hardware cursor. |
| Drag without a bitmap | No effect. | No effect: no switch is due, so nothing is uploaded. |
| Drag bitmap in another colour space (`B_CMAP8`, `B_RGB16`, ...) | Hardware cursor; bitmap not shown. | Same: the software cursor couldn't show the bitmap either. |
| Cursor change while hidden or obscured | Hardware cursor shown again. | Stays hidden until `SetCursorVisible(true)`, or until the mouse moves for an obscured cursor. |
| Startup (`Desktop::Init()`: `SetCursor(NULL)`, then `SetCursorVisible(true)`) | Shown by `SetCursor()`. | Uploaded hidden, then shown by `SetCursorVisible(true)`. |
| No cursor hooks (most drivers) | Software cursor. | Same. `SetCursor()` and `SetDragBitmap()` hold the exclusive lock where they didn't before. |

### Behavior changes

- Drags with a bitmap use the software cursor on all drivers, as they did before hardware cursors existed. The
  cursor is uploaded again when the drag ends.
- A hidden or obscured hardware cursor stays hidden across cursor changes. This matches the software cursor.
- With 0001 and 0002, captures read most of the screen from the back buffer, which never contains the software
  cursor or drag bitmap. In the areas read from the front buffer they still contain them, as before 0001.

## 4. Possible follow-ups (not in the patches)

- **Combined cursor in hardware.** Instead of falling back to software, the hardware could be given
  `fCursorAndDragBitmap` when it is small enough. The NVIDIA cursor goes up to 256×256, but drag bitmaps are often
  bigger. The combined bitmap is premultiplied by `_AdoptDragBitmap()` and its hot spot is shifted.
- **Drag bitmap in captures.** `DrawingEngine::ReadBitmap(drawCursor = true)` composites only `Cursor()`, never the
  drag bitmap. That is the same before and after these patches.

## 5. Verification

What was done:

- **0001 is unchanged** from the OBS session's copy.
- **Syntax-checked (not compiled).** clang 18 (`--target=x86_64-unknown-haiku -fsyntax-only -Wall`) was run against
  the Haiku tree's headers and the app_server include paths from its Jamfile, on these files:
  - `AccelerantHWInterface.cpp`
  - `HWInterface.cpp`
  - `DrawingEngine.cpp`
  - `ServerApp.cpp`
  - `EventDispatcher.cpp`
  - `Desktop.cpp`

  Their warnings are identical to unpatched master, and there are no errors from app_server code. The host's C++
  library headers produced unrelated errors.
- **The series applies** with `git am` to master `b9fdc8a9e6` and reproduces the tested tree.
- **Reviewed against the source.** Every caller of the changed functions and every lock taken on those paths was
  traced. A separate AI review pass, which hadn't written the patches, checked the first version. It found:
  1. the colour-space gap in `_DragBitmapShown()`;
  2. the stale-patch problem behind the revised 0002;
  3. the unlocked `HWInterface::SetDragBitmap()`;
  4. the single-buffered case of `_DrawSoftwareCursor()`;
  5. two inaccuracies in a commit message.

  All of these are fixed in this version. It found no deadlock, lock inversion or use-after-free.

What has **not** been done yet: building `app_server` with jam, and running it. The tester will install it on the
RTX 5070 machine (with the hardware-cursor accelerant) and add results here.

### Suggested test plan

1. **Drag with a hardware cursor.** Drag one icon, then several, in Tracker: the icon should follow the pointer.
   Drop it and check that no icon or pointer is left behind, and that the hardware cursor is back (it moves as
   smoothly as before the drag). Change cursor shape during a drag by passing over a text field or a window
   border.
2. **Drags without a bitmap** (text selection drags in StyledEdit, for example): no visible change.
3. **Hidden cursor.** Type in Terminal or StyledEdit (the cursor is obscured), and move over a link or text area
   without touching the mouse. The pointer should stay hidden until the mouse moves. An app that calls
   `HideCursor()` should keep it hidden while its cursor changes.
4. **No hardware cursor.** VESA, or nvidia-haiku with `hardware_cursor false`: drags and cursors behave exactly as
   before.
5. **`B_SET_CURSOR_SHAPE` drivers** (intel_extreme): drag with a `BCursor`-style cursor.
6. **With 0001 and 0002:**
   - `screenshot` and `screenshot -m` with the pointer over the desktop or an ordinary window: no pointer, then
     exactly one.
   - Over a constantly redrawing window, with the software cursor: no stale or erased square where the pointer is.
     A software pointer there may be captured, as before 0001.

## 6. The patches

`0001-…`, `0002-…` and `0003-…` next to this report apply with `git am`, in order, to master `b9fdc8a9e6`.
