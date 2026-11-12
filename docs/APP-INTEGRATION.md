# Application integration

What applications need to change to use Quasar on Renga. Changes to the
applications are made in their own repositories; this file records what the
driver offers and expects.

## OBS Studio (`Techniix/OBS-Studio_HaikuOS`)

- Open driver bug: adding a Screen Capture source hangs the graphics thread
  on a timeline fence (`docs/event-sync.md`, Open issues). No application
  change is expected for this.
- OBS's `renga` branch tracks its driver requests as Q-1 to Q-4 in its
  `docs/haiku/RENGA-REQUESTS.md`: Q-1 the hang, Q-2 shared GPU textures,
  Q-3 a hardware encode back end, Q-4 quiet logging.
- `obs-haiku-gl-probe` on OBS's `renga` branch follows libobs' per-frame
  call order for this case, with `--stall` reporting the stuck GL call and
  switches that remove one part of the load each (`--no-stage`,
  `--no-stream`, `--no-second-display`, `--no-handoff`, `--fences`,
  `--fps 0`). `tools/renga-test.sh run` runs it with `--verbose --stall 5`
  and, when it fails, once per switch and once per driver-side setting.
- Q-4: the `VideoStreamsWsi:` trace and NVK's SM-version notice are now
  printed only with `VIDEOSTREAMSWSI_DEBUG` and `NVK_NVRM_DEBUG`.
- Capture (OBS O-10, Quasar R-2): OBS needs a handle it can import with EGL
  or Vulkan external memory, the damaged region and a timestamp per frame.
- Hardware encoding (Q-3): through the Media Kit's `VideoAcceleration.h`
  back end, which FFmpeg's plug-in already uses.

## Sunshine (`Techniix/Sunshine_HaikuOS`)

- Captures with `BScreen::ReadBitmap()` today. The GPU capture provider on
  its roadmap maps onto request R-2 in `RENGA-REQUESTS.md`.
- Hardware encoding depends on the hardware video investigation.

## Eglantine (RengaPorts)

- Presents through VideoStreamsWsi's headless surface and a `BitmapHook`
  today. It moves to the Renga WSI once that exists.
- Driver settings that help only Eglantine go in its per-application
  profile.

## All Vulkan and Zink applications: the Renga WSI

On this branch presentation goes through NVK's swapchains with the RengaWsi
surface layer instead of VideoStreamsWsi (`docs/RENGA-WSI.md`). Applications
need no change: `vkCreateHeadlessSurfaceEXT` plus a cast to
`VKLayerSurfaceBase` and `SetBitmapHook()` work as before. Differences an
application may notice:

- only `VK_FORMAT_B8G8R8A8_UNORM` and `_SRGB` are offered on such surfaces;
- `currentExtent` is the window's size from the hook;
- `RENGA_WSI_DEBUG=1` prints surface and swapchain creation.
