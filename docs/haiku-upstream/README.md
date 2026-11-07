# app_server patches for Haiku

Three patches to Haiku's app_server. They are not part of this driver and are **not** applied by `Build.sh`. They are written up for Haiku's developers, who may reuse, rewrite or discard them, since Haiku accepts no AI-generated code. They also let you test on your own machine.

| Patch | From | Report |
| --- | --- | --- |
| `0001-…serve-ReadBitmap-from-the-back-buffer…` | OBS Studio port ([Techniix/OBS-Studio_HaikuOS](https://github.com/Techniix/OBS-Studio_HaikuOS), `docs/haiku/upstream/`, copied unchanged) | `app_server-readbitmap-report.md` (copied unchanged) |
| `0002-…ReadBitmap-read-front-buffer-areas-with-the-cursor` | this repository | `app_server-hardware-cursor-report.md` |
| `0003-…show-drag-bitmaps-with-a-hardware-cursor` | this repository | `app_server-hardware-cursor-report.md` |

Apply them in order with `git am` to haiku master `b9fdc8a9e6`. `TESTING.md` shows how to build and install them on Haiku, and how to go back.

0002 replaces part of 0001: it removes `HWInterface::SoftwareCursorFrame()`, which 0001's report describes. Read that part of 0001's report together with 0002.
