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
