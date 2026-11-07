# Testing the app_server patch series on your machine

The five patches in this directory change app_server:

- **0001** (from the OBS Studio port) makes screen capture read from the back buffer.
- **0002** corrects how 0001 handles the software cursor in the areas it reads from the frame buffer.
- **0003** shows drag bitmaps with a hardware cursor and keeps a hidden cursor hidden.
- **0004** and **0005** (from the OBS Studio port) fix out-of-bounds writes in `ReadBitmap()`: one that 0001 introduced, and two that were already there.

The procedure is the OBS session's (`docs/haiku/upstream/app_server-readbitmap-testing.md` in the OBS repo), applied to all five patches. If you already built and installed an earlier set, start again from step 2 with a fresh clone.

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
for f in $P/0001-*.patch $P/0002-*.patch $P/0003-*.patch $P/0004-*.patch $P/0005-*.patch; do
	git apply --check "$f" && git apply "$f" && echo "applied $(basename $f)" || { echo "FAILED: $f"; break; }
done
git status --short     # 8 modified files, all under src/servers/app
```

All five lines should say `applied`. The patches were made against master of 2026-11-06 and also apply to master of 2026-11-07. If a `--check` fails, send the error and the hrev and they'll be rebased.

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

pkgman should replace **one package, `haiku`**. The new version ends in `_hrevNNNNN_dirty-1` ("dirty" because the patches are uncommitted), which sorts after your installed `_hrevNNNNN-1`. If pkgman wants to change anything else, answer **no** and send the output. Then reboot.

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

**Bitmap edges (0004, 0005)**

9. Run `screenshot -m` with the pointer at the very bottom and right edges of the screen, and partly off them. The image should be correct, with the visible part of the pointer, and nothing should crash (check that `screenshot` and OBS keep running).
10. In OBS, capture a region (not the whole screen) and move the pointer across its bottom and right edges with Capture Cursor on.

**Unaffected cases**

11. With the software cursor (step 7's setting), check drags, typing and screenshots once more. Everything should behave as before.

Send back the results and the hrev, to be added to the reports.

## 5. Going back to the official package

```sh
pkgman full-sync
```

Or use the boot menu (section 0).
