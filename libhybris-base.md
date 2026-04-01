# libhybris Build Notes

## Base Version

The patch `libhybris-tawc.patch` applies against:

- **Repo:** `https://github.com/Linux-on-droid/libhybris.git`
- **Branch:** `lindroid-21`
- **Head commit:** `fc5954b Cherry-pick TLS thunk patcher from lindroid-drm`
  (cherry-picked from `lindroid-drm` branch, original commit `75be4aa`)
- **Parent commits:**
  ```
  fc5954b Cherry-pick TLS thunk patcher from lindroid-drm
  419f3ff compat: ui: fix compliation on Android 14 QPR3
  87df54e hooks: Fix qsort incompatible-pointer-types
  72a389e initialize gralloc incase it wasn't
  516353a test_vulkan: Add xdg_wm_base support
  ```

The upstream `libhybris/libhybris` master does NOT have the TLS thunk patcher.
The lindroid fork adds it. Our patch builds on top of the lindroid fork's thunk
patcher with the bionic_tls allocation fix.

## Android Headers

libhybris needs Android headers at build time. These are NOT the NDK headers —
they're the internal AOSP headers for bionic, system, and hardware interfaces.

- **Source:** `https://github.com/AJAndroid/android-headers` (formerly by AJAndroid, Squishy123, others)
- **Version used:** Android 15 headers, with the version bumped to 16 in the
  `android-config.h` file (changed the version number constant)
- **Location in chroot:** `/usr/local/include/android/`
- **Installed via:** The headers come as a pkg-config package (`android-headers`).
  Clone the repo, run `autoreconf -i && ./configure --prefix=/usr/local && make install`

### Header fixes needed for GCC

The Android headers are written for Clang and need fixes to compile with GCC:

1. **`_Nonnull`/`_Nullable` annotations** — Clang-specific, not defined in GCC.
   Add to `android-config.h`:
   ```c
   #ifndef __clang__
   #  ifndef _Nonnull
   #    define _Nonnull
   #  endif
   #  ifndef _Nullable
   #    define _Nullable
   #  endif
   #endif
   ```

2. **Scoped enum syntax** — `enum Foo : int32_t` is C++ syntax that GCC rejects
   in C mode. Fix by removing the `: int32_t` suffix in:
   - `vndk/hardware_buffer.h`
   - `android/hardware_buffer.h`

The script `fix-android-headers.sh` automates both fixes.

## Configure Options

```
./configure \
    --enable-arch=arm64 \
    --enable-adreno-quirks \
    --enable-property-cache \
    --with-default-hybris-ld-library-path=/vendor/lib64/egl:/vendor/lib64/hw:/vendor/lib64:/system/lib64 \
    --prefix=/usr/local
```

- `--enable-arch=arm64` — aarch64 target
- `--enable-adreno-quirks` — Qualcomm Adreno GPU workarounds
- `--enable-property-cache` — cache Android system properties
- `--with-default-hybris-ld-library-path` — where libhybris's bionic linker
  searches for Android `.so` files. Must include `/vendor/lib64/hw/` for
  gralloc/mapper HAL passthrough loading.

## Build Dependencies (Arch Linux ARM)

```
pacman -S --noconfirm --needed base-devel git libtool wayland pkg-config
```

## Build Process

```bash
cd /root/libhybris-lindroid/hybris
./autogen.sh
./configure [options above]
make -j4
make install  # or install individual components
ldconfig
```

Not all components build cleanly (e.g. `libsync` has header issues on Android
16). The critical components are: `common` (libhybris-common.so), `egl`, `glesv2`.
The build script `build-libhybris-lindroid` handles partial installs.

## Runtime Environment

To use libhybris from the chroot:

```bash
export HYBRIS_PATCH_TLS=1    # required for stock Android (enables TLS thunk patcher)
export LD_LIBRARY_PATH=/usr/local/lib
```

`HYBRIS_PATCH_TLS=1` enables the TLS thunk patcher for ALL loaded bionic
libraries. Can also be set to a colon-separated list of library filenames to
limit patching (e.g. `HYBRIS_PATCH_TLS=libGLES_mali.so`).

## What the Patch Changes

See the patch file for details. Summary of the three changed files:

### `hybris/common/hooks.c`
- **bionic_tls allocation:** Changed `tls_hooks[16]` to a struct with a
  `bionic_tls_ptr` pre-slot. Lazily allocates 16KB zero-filled buffer per thread.
  This is what makes stock Android work — bionic's slot -1 TLS access finds
  valid memory instead of crashing.
- **pthread_create wrapper:** Wraps thread start routines to initialize
  bionic_tls before bionic code runs on new threads.
- **android_dlopen_ext namespace bypass:** Strips `ANDROID_DLEXT_USE_NAMESPACE`
  flag and falls back to regular dlopen. (Attempted fix for gralloc — doesn't
  fully solve it because bionic→bionic calls bypass hooks.)
- **android_load_sphal_library hook:** Redirects to regular android_dlopen.
  (Same caveat — hooks aren't called for bionic→bionic resolution.)
- **android_get_exported_namespace:** Returns NULL to disable linker namespace
  lookups. (Partially effective — see gralloc-problem.md.)

### `hybris/common/q/dlfcn.cpp`
- **`__loader_android_get_exported_namespace`:** Returns nullptr. This is the
  linker-internal version that bionic code resolves to directly.

### `hybris/common/q/linker.cpp`
- **`do_dlopen` namespace handling:** Ignores `ANDROID_DLEXT_USE_NAMESPACE`
  flag in extinfo, uses default namespace instead.

The namespace/gralloc changes (last three items) are investigatory — they don't
fully solve the gralloc problem but document the approaches tried.
