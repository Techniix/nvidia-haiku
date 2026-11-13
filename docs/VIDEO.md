# Hardware video on Renga

What the RTX 5070's video engines can be used for with Quasar, what is
missing, and the plan for Renga's Media Kit.

## Summary

| | State | Notes |
| --- | --- | --- |
| H.264 decode through Vulkan Video (NVK) | Written, not built or run | Mesa 26.3's NVK, experimental: `NVK_EXPERIMENTAL=video` |
| H.265, AV1, VP9 decode | Not available | Not in NVK yet; upstream reports its H.265 work wedging GSP |
| Encode (NVENC) | Not possible for now | No public programming interface; see below |
| Renga Media Kit back end (`VideoAcceleration.h`) | Planned | On top of Vulkan Video; see below |

## Decode

NVK gained Vulkan Video decode in Mesa 26.3 (H.264 only, behind
`NVK_EXPERIMENTAL=video`, Turing and later). The decoding itself is NVK's
shared code (`src/nouveau/vulkan/rust/video`). What the kernel-mode back end
provides is a channel on the video decoder engine and the decoder class.
Quasar's Mesa branch `renga-26.3` adds that to the RM back end:

- `cls_vdec` is the newest NVDEC class RM lists (`NVCFB0_VIDEO_DECODER` on
  the RTX 5070), and `has_video` follows from it.
- A video decode queue gets its own channel on `NV2080_ENGINE_TYPE_NVDEC0`,
  with the decoder object allocated on it. Submission and synchronisation
  work as on the graphics channel.

To try it, the application runs with `NVK_EXPERIMENTAL=video`. NVK then
reports a video decode queue family and `VK_KHR_video_decode_queue`,
`VK_KHR_video_decode_h264`. FFmpeg's Vulkan hwaccel can use it:

```sh
NVK_EXPERIMENTAL=video ffmpeg -init_hw_device vulkan=gpu -filter_hw_device gpu \
    -hwaccel vulkan -hwaccel_output_format vulkan -i clip.mp4 \
    -vf hwdownload,format=nv12 -f null -
```

`tools/renga-test.sh run --video` does that with a generated clip and
checks every frame against FFmpeg's software decoder. It runs last, after
the rest of the report is packed, because a fault in the decoder can hang
the GPU.

Open on the hardware:

- NVK binds the decoder with `SET_OBJECT` and the Turing class (`NVC5B0`)
  on every GPU, as on nouveau. Whether RM's decoder channel on Blackwell
  accepts that is not known yet.
- Decoding needs `-Dvideo-codecs` to include `h264dec`; `Build.sh` passes
  `all_free,h264dec`. H.264 is a patent-encumbered format, which is why
  Mesa leaves it out by default.

## Encode

There is no way to use NVENC from an open driver at the moment. NVIDIA
publishes the encoder's class IDs, but not its command interface or picture
parameter layouts. Nouveau and NVK have never implemented it, and NVIDIA's
encoding library is a closed Linux binary that does not run on Haiku.

So OBS Studio and Sunshine encode on the CPU for now (x264, SVT-AV1 or
OpenH264, whichever their Renga builds include). Capture and colour
conversion can still run on the GPU.

If NVIDIA publishes the NVENC interface, as it did for NVDEC's
(`nvdec_drv.h`, used by NVK), the same RM channel work as for decode would
apply.

## Media Kit back end (plan)

Renga's `VideoAcceleration.h` (Media Kit phase 5) loads video back ends as
add-ons from `add-ons/media/video_accelerators`. Its decoder takes whole
compressed packets, and NVIDIA's hardware wants parsed parameters (slice
headers, SPS/PPS), so a back end needs a bitstream parser.

The plan is a vendor-neutral back end on top of Vulkan Video, using FFmpeg's
Vulkan hwaccel for parsing and decoding, rather than one tied to NVIDIA:

- It works with any Vulkan driver with video decode, so the LinuxKPI DRM
  drivers can use it too.
- FFmpeg's parsers are mature; writing new ones would not be.
- It depends on Renga's FFmpeg being built with Vulkan
  (`--enable-vulkan`); that is to be checked with Renga.

Decoded surfaces come back as NV12 (8-bit) and later P010 (10-bit), which
is what `VideoAccelSurface` expects. Zero-copy display of decoded frames
waits on Renga's GPU stage (F-47).
