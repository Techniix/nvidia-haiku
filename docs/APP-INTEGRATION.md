# Application integration

What applications need to change to use Quasar on Renga. Changes to the
applications are made in their own repositories; this file records what the
driver offers and expects.

## OBS Studio (`Techniix/OBS-Studio_HaikuOS`)

- Open driver bug: adding a Screen Capture source hangs the graphics thread
  on a timeline fence (`docs/event-sync.md`, Open issues). No application
  change is expected for this.
- `obs-haiku-gl-probe` passes today and does not reproduce the Screen
  Capture case (a mapped `GL_PIXEL_UNPACK_BUFFER` upload of 2560x1080 BGRX at
  30 fps while a second FBO-backed display is created and resized). The
  probe should gain that case; Quasar's `tests/` gains the same pattern.
- Later, once the driver provides them: shared textures between processes,
  hardware encoding, capture from an exported scanout buffer.

## Sunshine (`Techniix/Sunshine_HaikuOS`)

- Captures with `BScreen::ReadBitmap()` today. The GPU capture provider on
  its roadmap maps onto request R-2 in `RENGA-REQUESTS.md`.
- Hardware encoding depends on the hardware video investigation.

## Eglantine (RengaPorts)

- Presents through VideoStreamsWsi's headless surface and a `BitmapHook`
  today. It moves to the Renga WSI once that exists.
- Driver settings that help only Eglantine go in its per-application
  profile.
