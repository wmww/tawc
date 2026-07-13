# ImageMagick default/fontconfig font resolution fails ("invalid stream operation") in the Arch guest

Found by the `cli-media-tools` usecase test on the physical OnePlus
(device 50f4ca18), Arch tawcroot install, 2026-07-13.
ImageMagick 7.1.2-26 Q16-HDRI aarch64 (Arch package), imagemagick +
deps liblqr/libraqm installed via the cache proxy.

## Symptom

Any ImageMagick text operation that relies on the *default* font or a
*fontconfig family-name* lookup fails:

```
$ magick grad.png -annotate +0+5 "hi" out.png        # no -font
magick: unable to read font ` (invalid stream operation)' @ error/annotate.c/RenderFreetype/1665.
$ magick -background white label:"hi" out.png
magick: unable to read font ` (invalid stream operation)' @ error/annotate.c/RenderFreetype/1665.
$ magick -background white -size 200x caption:"hi" out.png   # same
$ magick montage a.png b.png -tile 2x1 out.png        # auto filename labels
montage: unable to read font ` (invalid stream operation)' @ error/annotate.c/RenderFreetype/1665.
```

Note the empty backtick: the resolved font path is an empty string, so
FreeType is handed `""` and returns "invalid stream operation". Passing a
family name is no better:

```
$ magick ... -font Helvetica ...        -> unable to read font `Helvetica (invalid stream operation)'
$ magick ... -font "Adwaita Sans" ...   -> unable to read font `Adwaita Sans (invalid stream operation)'
```

`montage` still writes a correctly-tiled image (dimensions right) but
exits 1 and omits the labels.

## What works

- Explicit font **file path** (any format):
  `-font /usr/share/fonts/Adwaita/AdwaitaMono-Bold.ttf` and
  `-font /usr/share/fonts/gnu-free/FreeSans.otf` both render fine.
- `type*.xml` **named** entries with an explicit `glyphs=` path:
  `-font DejaVu-Sans` works.
- The **`pango:` coder**: `magick -background white pango:"hi" out.png`
  renders fine.

## Layer: upstream/distro ImageMagick, NOT tawcroot

Ruled out the obvious causes:

- Not missing fonts. `/usr/share/fonts` has Adwaita + gnu-free out of the
  box; installing `ttf-dejavu` (fc-match default then → DejaVuSans.ttf)
  did **not** fix it — same error.
- fontconfig itself works: `fc-match`, `fc-match sans-serif`,
  `fc-match Helvetica` all resolve to real files from the CLI; the cache
  under `/var/cache/fontconfig` is populated; `fc-cache -f` changes
  nothing.
- Not a tawcroot syscall/interception problem: **`pango:` rendering works
  in the same process**, and pango uses fontconfig+freetype in-process.
  So in-process fontconfig+freetype is fine; only ImageMagick's own
  native font-match path (MagickCore annotate.c / `AcquireTypeInfo`
  fontconfig query) returns an empty file path.
- `libMagickCore` is linked against libfontconfig.so.1 and
  libfreetype.so.6, and `fontconfig`/`freetype` are in the build's
  DELEGATES list.

Conclusion: this is an ImageMagick-7.1.2-26-aarch64 (Arch) default-type /
fontconfig-family resolution quirk, independent of tawc. It should be
reproducible on a stock aarch64 Arch box with the same package and no
DejaVu/gsfonts pre-installed; a control run there would confirm and, if
so, this is purely an upstream/distro-packaging note, not a tawc bug.
(Could not run that control from here — physical target only.)

## User impact / discoverability

Low-to-moderate. `magick label:`, `caption:`, and a bare `-annotate`
(the most natural way a user adds text) all fail with a cryptic
empty-backtick error that does not hint at the fix. A user has to know to
pass an explicit `-font <file>`, use a `type.xml` name like
`-font DejaVu-Sans`, or use `pango:`. All non-text ImageMagick operations
(generate, identify, resize, PNG->JPEG, montage tiling) work perfectly.

## Rest of the usecase: PASS

- ImageMagick core: `gradient:` gen (800x600 PNG), `identify`, resize to
  200x150, PNG->JPEG (quality 90), montage tiling — all correct dims /
  formats / non-trivial sizes.
- ffmpeg (already installed in the guest, n8.1.2): synth
  `testsrc` 5s 640x360 H.264 in 0.48s; transcode to VP9/webm in 3.9s
  (user 6.6s across 7 cores => multi-threaded; 1.33x speed, not
  pathological); extract 3 PNG frames; `ffprobe` confirms h264/vp9,
  640x360, duration 5.0 on both. No audio devices touched.

## Suggested fix options

1. Docs/caveat only: note in the media-tools guidance that IM text ops
   need an explicit `-font <file>` (or `pango:`) in the minimal guest.
2. If confirmed upstream, file with Arch/ImageMagick; nothing for tawc to
   change.
