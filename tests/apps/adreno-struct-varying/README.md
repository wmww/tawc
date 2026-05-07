# adreno-struct-varying — Qualcomm Adreno GLSL bug reproducer

Minimal standalone reproducer for a long-standing bug in Qualcomm's
Adreno Android GLES driver: a **struct-typed varying passed into a
function in the fragment shader** silently produces wrong values
(typically all zeros).

This was the actual cause of the "GTK4 ≤ 4.19 renders blank widgets
on Android/Adreno" symptom we hit in tawc. GTK4's GSK GPU renderer
used `Rect` / `RoundedRect` struct varyings shared via `common.glsl`
across most of its shader programs. On Adreno, every fragment came
out (0,0,0,0); on Mesa it was fine. Upstream GTK fixed it in
[`00beb04753`](https://gitlab.gnome.org/GNOME/gtk/-/commit/00beb04753ae347309848c2f09d51430f1b56c21)
(GTK 4.20.0, fixes
[#7531](https://gitlab.gnome.org/GNOME/gtk/-/issues/7531)) by
replacing the struct varyings with `vec4[1]` / `vec4[3]` arrays —
same data, just wrapped differently.

## The pattern

Broken (mode=struct):

```glsl
// vertex shader:
struct Rect { vec4 col; };
out Rect v_rect;
// ... assign v_rect.col, set gl_Position ...

// fragment shader:
struct Rect { vec4 col; };
in Rect v_rect;
vec4 extract(Rect r) { return r.col; }
void main() { frag = extract(v_rect); }
```

Workaround (mode=array):

```glsl
// vertex shader:
out vec4 v_rect[1];
// ... v_rect[0] = colour ...

// fragment shader:
in vec4 v_rect[1];
vec4 extract(vec4 r[1]) { return r[0]; }
void main() { frag = extract(v_rect); }
```

The struct version compiles and links cleanly on Adreno, no
`glGetError`, but the function call returns garbage.

## Files

| File | Build | Runs as |
|------|-------|---------|
| `adreno-struct-varying.c` | Wayland + glibc, see `build.sh` | a Wayland client (xdg_toplevel) — used in tawc's chroot via libhybris |
| `adreno-struct-varying-bionic.c` | NDK aarch64 clang, `-lEGL -lGLESv3` | direct `adb shell` binary, no Wayland (EGL pbuffer) |

Both implement the same `--mode={struct,array}` switch and use
identical shader sources. They print the centre pixel after one
draw and exit 0 (GREEN), 2 (setup failure), or 3 (BROKEN — wrong
colour). The Wayland binary needs an xdg_shell-capable compositor
running; the bionic one needs nothing.

## Build

Wayland version (run from a host or chroot with libwayland +
wayland-protocols):

```sh
bash build.sh
./adreno-struct-varying --mode=struct
./adreno-struct-varying --mode=array
```

Bionic version (Android NDK):

```sh
NDK=$HOME/Android/Sdk/ndk/27.2.12479018
$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android24-clang \
    adreno-struct-varying-bionic.c -o adreno-struct-varying-bionic \
    -lEGL -lGLESv3
adb push adreno-struct-varying-bionic /data/local/tmp/
adb shell '/data/local/tmp/adreno-struct-varying-bionic --mode=struct'
adb shell '/data/local/tmp/adreno-struct-varying-bionic --mode=array'
```

## Verified configurations (2026-05)

```
GL_VENDOR:   Qualcomm
GL_RENDERER: Adreno (TM) 660
GL_VERSION:  OpenGL ES 3.2 V@0530.0 (GIT@5a9022f91f, Date:07/26/21)
```

| Setup                                | mode=struct          | mode=array        |
|--------------------------------------|----------------------|-------------------|
| Mesa llvmpipe (Linux x86_64 host)    | GREEN ✓              | GREEN ✓           |
| Adreno 660, libhybris/glibc/Wayland  | BROKEN R0 G0 B0 A0   | GREEN R0 G255 B0 A255 |
| Adreno 660, **bionic** + `adb shell` | BROKEN R0 G0 B0 A0   | GREEN R0 G255 B0 A255 |

Hardware/OS for the Adreno rows: OnePlus 9, Adreno 660, LineageOS
21 (Android 14), driver build dated 2021-07-26.

The bionic row is the load-bearing one: identical behaviour with
nothing in the picture but bionic's libc + Android's stock EGL/GLES
(no libhybris, no glibc, no chroot, no Wayland, no compositor) means
**the bug is purely in Qualcomm's vendor Adreno driver**, not in any
layer above it.

## Useful for

1. Verifying the Adreno bug is still present on a new device /
   firmware before working around it.
2. Filing an upstream bug with Qualcomm (the bionic build has no
   external deps and reproduces in ~150 lines).
3. Sanity-checking that a libhybris change didn't somehow start
   masking or worsening this — both binaries should give identical
   results.
