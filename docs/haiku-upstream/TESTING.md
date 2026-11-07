# Testing the app_server patch series on your machine

The three patches in this directory change app_server:

- **0001** (from the OBS Studio port) makes screen capture read from the back buffer.
- **0002** corrects how 0001 handles the software cursor in the areas it reads from the frame buffer.
- **0003** shows drag bitmaps with a hardware cursor and keeps a hidden cursor hidden.

The procedure is the OBS session's (`docs/haiku/upstream/app_server-readbitmap-testing.md` in the OBS repo), applied to all three patches. If you already built and installed 0001 alone, start again from step 2 with all three.

None of this has been built or run yet. Haiku's boot menu can return you to the system exactly as it was, so read section 0 first.

## 0. Recovery, in case the new app_server doesn't start

1. Reboot and hold **Shift** (or Space) until the boot menu appears.
2. **Select boot volume** → your Haiku volume → **Select state**.
3. Pick the state from before the install → **Return to main menu** → **Continue booting**.

To make it permanent: `pkgman full-sync`.

## 1. Find your exact Haiku revision

```sh
uname -v
```

Note the `hrevNNNNN`; replace `hrevNNNNN` below with it.

## 2. Get the matching source and apply the series

```sh
mkdir -p ~/haiku-dev && cd ~/haiku-dev
git clone --depth 1 --branch hrevNNNNN https://review.haiku-os.org/haiku.git
cd haiku
P=~/nvidia-haiku/docs/haiku-upstream
git apply --check $P/0001-*.patch && git apply $P/0001-*.patch
git apply --check $P/0002-*.patch && git apply $P/0002-*.patch
git apply --check $P/0003-*.patch && git apply $P/0003-*.patch
```

The patches were made against master of 2026-11-06. If a `--check` fails, send the error and the hrev and they'll be rebased.

## 3. Configure, build, install

```sh
which jam || pkgman install jam
mkdir generated.x86_64 && cd generated.x86_64
../configure --target-arch x86_64
jam -q -j$(nproc) app_server      # quick compile check
jam -q -j$(nproc) haiku.hpkg
find . -name haiku.hpkg
pkgman install ./objects/haiku/x86_64/packaging/packages/haiku.hpkg
```

pkgman should replace **one package, `haiku`**. If it wants to change anything else, answer **no** and send the output. Then reboot.

Use the nvidia-haiku build with the hardware cursor (branch `rtx5070-hw-cursor`) for the cursor tests.

## 4. Test

**Drags (0003)**

1. In Tracker, drag one icon, then a selection of several. The icons should follow the pointer.
2. Drop them. No icon or pointer should be left behind, and the pointer should move as smoothly as before the drag (the hardware cursor is back).
3. During a drag, pass over a text field or a window border, so the cursor shape changes. The icon should stay visible.
4. Drag selected text in StyledEdit (a drag without a bitmap). Nothing should look different from before.

**Hidden cursor (0003)**

5. Type in Terminal: the pointer hides. Without touching the mouse, let a window open or move under it (Terminal's `ls -R /` scrolling, for example). The pointer should stay hidden until you move the mouse.

**Screen capture (0001 with 0002)**

6. Run `screenshot` and `screenshot -m` with the pointer over the desktop or an ordinary window. The first image should have no pointer, the second exactly one.
7. Put `hardware_cursor false` in `~/config/settings/kernel/drivers/nvidia_gsp` and restart app_server, so the software cursor is in use. Take screenshots with the pointer over a window that redraws constantly (Terminal running `top`). There should be no stale or erased square where the pointer is; that is what 0002 is about. A pointer may still show up over that window, as it always did.
8. In OBS, run Screen Capture with the "Tear-free" method at 30 FPS. The pointer should be smooth, and Capture Cursor on/off should show or hide it. Then run the OBS session's other checks (redrawing windows, direct windows, resolution change).

**Unaffected cases**

9. With the software cursor (step 7's setting), check drags, typing and screenshots once more. Everything should behave as before.

Send back the results and the hrev, to be added to the reports.

## 5. Going back to the official package

```sh
pkgman full-sync
```

Or use the boot menu (section 0).
