# Usecase test: media CLI tools (ImageMagick, ffmpeg)

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user batch-processes images and transcodes video from the command line.

## Prerequisites

- Cache proxy up (README step 6).
- `pacman -S --noconfirm imagemagick`. Then check the download size of
  `pacman -S ffmpeg` before confirming; if the dependency pull is
  unreasonable (>~500 MB), skip the ffmpeg half and note that in the
  result — ImageMagick alone is still a meaningful pass.

## Steps

Work under `/root/usecase-media/`.

1. ImageMagick: `magick -size 800x600 gradient:blue-red grad.png`;
   `identify` it; resize to 200x150; convert PNG→JPEG; annotate with
   text (needs fonts — if annotation fails on missing fonts, record how
   discoverable the failure is for a user); montage two images.
   Verify outputs with `identify` (dimensions, format) and non-trivial
   file sizes.
2. ffmpeg (if installed): synthesize a 5 s test clip
   (`ffmpeg -f lavfi -i testsrc=duration=5:size=640x360:rate=30 test.mp4`),
   transcode it to another codec/container, extract 3 frames as PNGs,
   `ffprobe` both files and check duration/stream metadata. This is
   CPU-only work — no audio devices and no GPU codecs are expected to be
   involved (there is no audio bridge at all: plans/audio.md).
3. Time the transcode; grossly pathological slowness (minutes for 5 s of
   360p) would be a finding.

## Expected results

- All conversions produce valid outputs with expected dimensions,
  formats, and durations; multi-threaded encode works.

## Known issues / caveats

- Anything audio-related is out of scope — no audio bridge exists yet
  (plans/audio.md). ffmpeg *file-to-file* audio streams (testsrc with
  sine audio) should still encode fine since no device is touched.

## Cleanup

Remove `/root/usecase-media/`, `pacman -Rns imagemagick` (and `ffmpeg`
if installed).

## Run log (2026-07-13, physical OnePlus 50f4ca18, Arch tawcroot)

Outcome: PROBLEMS (one finding). ffmpeg + all non-text ImageMagick
operations pass; ImageMagick's default/fontconfig text rendering fails.

- Setup: ImageMagick installed via cache proxy (imagemagick + liblqr +
  libraqm, small pull). ffmpeg n8.1.2 was already present in the guest.
- ImageMagick core: `gradient:` gen (800x600 PNG), `identify`, resize
  200x150, PNG->JPEG q90, montage tiling (420x160) — all correct
  dims/formats/non-trivial sizes.
- ImageMagick text FAILS: `-annotate` (no `-font`), `label:`, `caption:`,
  and montage auto-labels all error `unable to read font
  ` (invalid stream operation)'`. Not a missing-fonts issue (fonts
  present; installing ttf-dejavu didn't help; `fc-match` works from CLI).
  Isolated to IM's own font-match path: explicit `-font <file>`,
  `type.xml`-named `-font DejaVu-Sans`, and the `pango:` coder all render
  fine — and `pango:` proves in-process fontconfig+freetype works, so it
  is NOT a tawcroot syscall problem. Attributed to upstream/distro
  ImageMagick (aarch64 7.1.2-26). See
  issues/usecase_tests/imagemagick-default-font-resolution-fails-invalid-stream-operation.md
- ffmpeg: synth testsrc 5s 640x360 H.264 (0.48s); transcode VP9/webm
  (3.9s, multi-threaded over 7 cores, 1.33x, not pathological); extract 3
  PNG frames; `ffprobe` confirms h264/vp9 640x360 dur 5.0 on both. No
  audio touched.
- Cleanup done: removed /root/usecase-media, `pacman -Rns imagemagick
  ttf-dejavu` (+ orphan deps). ffmpeg left in place (pre-existing).
