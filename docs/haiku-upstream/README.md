# app_server patches for Haiku

Five patches to Haiku's app_server. They are not part of this driver and are **not** applied by `Build.sh`. They are written up for Haiku's developers, who may reuse, rewrite or discard them, since Haiku accepts no AI-generated code. They also let you test on your own machine.

| Patch | From | Report |
| --- | --- | --- |
| `0001-…serve-ReadBitmap-from-the-back-buffer…` | OBS Studio port ([Techniix/OBS-Studio_HaikuOS](https://github.com/Techniix/OBS-Studio_HaikuOS), `docs/haiku/upstream/`, copied unchanged) | `app_server-readbitmap-report.md` (copied unchanged) |
| `0002-…ReadBitmap-read-front-buffer-areas-with-the-cursor` | this repository | `app_server-hardware-cursor-report.md` |
| `0003-…show-drag-bitmaps-with-a-hardware-cursor` | this repository | `app_server-hardware-cursor-report.md` |
| `0004-…ReadBitmap-never-read-more-than-fits-into-the-bitmap` | OBS Studio port (copied unchanged) | `app_server-readbitmap-report.md` |
| `0005-…ReadBitmap-clip-the-cursor-to-the-bitmap` | OBS Studio port (copied unchanged) | `app_server-readbitmap-report.md` |

The OBS files are copied from `docs/haiku/upstream/` in the OBS repository at commit `998db8f`; that repository also holds a copy of 0002. The report there covers 0001, 0002, 0004 and 0005.

- **0002** corrects 0001's software-cursor handling. **0004** fixes an out-of-bounds write that 0001 introduced, plus a related one that was already there. Both can be squashed into 0001.
- **0005** fixes an out-of-bounds write that already existed in `ReadBitmap()`'s `drawCursor` compositing. It stands alone.
- **0003** (drag bitmaps and hidden cursors with a hardware cursor) touches only `AccelerantHWInterface`. It is independent of the others: 0004 and 0005 apply before or after it, and both orders give the same tree.

`build-fix/` holds Haiku's own commit `db67a42`, unchanged, which lets `haiku.hpkg` build natively without a 32-bit libgcc; it is only needed for revisions that predate it (see `TESTING.md`).

All five apply in order (0001 to 0005), with `git am` or `git apply`, to haiku master `b9fdc8a9e6` and to master `313814a` of 2026-11-07. `TESTING.md` shows how to build and install them on Haiku, and how to go back.
