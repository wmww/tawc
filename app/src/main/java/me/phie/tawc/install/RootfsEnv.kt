package me.phie.tawc.install

import me.phie.tawc.GraphicsBackend
import me.phie.tawc.Settings

/**
 * Environment variables passed to the in-rootfs `bash -lc` shell.
 *
 * Each install method's [InstallationMethod.startInside] spawns the
 * shell under `/usr/bin/env -i` so nothing Android (or proot/tawcroot)
 * leaks through; this map is the entire env the in-rootfs world sees
 * before `/etc/profile` runs. Distro `/etc/profile` files set their own
 * `PATH` unconditionally, so the in-rootfs PATH ultimately comes from
 * the distro — we set it here as well only to give scripts that read
 * `$PATH` before profile.d completes a sane fallback.
 *
 * Per-method tweak: `MOZ_DISABLE_*_SANDBOX` is set under proot only.
 * Firefox's per-subprocess sandboxes SIGSEGV under proot's ptrace
 * tracer; tawcroot/chroot have no tracer and let the sandbox come up
 * cleanly, so applying these vars there would weaken security for no
 * gain.
 *
 * Per-backend tweak: env diverges by [GraphicsBackend] (read fresh
 * from [Settings] on every spawn). Libhybris is the historical default
 * and keeps the libhybris dirs on `LD_LIBRARY_PATH`; gfxstream pins
 * Mesa's gfxstream Vulkan ICD via `VK_ICD_FILENAMES` and selects the
 * kumquat transport via `VIRTGPU_KUMQUAT=1`, and it deliberately keeps
 * libhybris off `LD_LIBRARY_PATH` so libhybris's `libvulkan.so.1`
 * doesn't shadow the distro vulkan-icd-loader. CPU forces software
 * rendering via `LIBGL_ALWAYS_SOFTWARE=1` + `GALLIUM_DRIVER=llvmpipe`
 * and leaves the Vulkan loader to pick up lavapipe on its own.
 */
internal object RootfsEnv {
    enum class Method { TAWCROOT, PROOT, CHROOT }

    fun build(method: Method): Map<String, String> =
        build(method, Settings.graphicsBackend)

    fun build(method: Method, backend: GraphicsBackend): Map<String, String> = buildMap {
        put("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin")
        put("HOME", "/root")
        put("TMPDIR", "/tmp")
        // Wayland socket is exposed inside the rootfs at /usr/share/tawc/
        // via the per-method bind of the host's <appData>/share/ dir
        // (the only thing we expose from the app data dir into rootfs
        // view — see notes/installation.md "/usr/share/tawc"). Wayland
        // clients honour absolute WAYLAND_DISPLAY directly, no /tmp
        // symlink needed.
        put("WAYLAND_DISPLAY", "/usr/share/tawc/wayland-0")
        put("XDG_RUNTIME_DIR", "/tmp")
        when (backend) {
            GraphicsBackend.LIBHYBRIS -> {
                // libhybris is laid down by [TawcInstaller] /
                // [LibhybrisInstallProvider] as real files under /usr/lib/hybris/
                // (a tawc-owned namespace — /usr/local/lib/ stays free for the
                // user's own installs). gl-shims first so the libGL/libGLESv2
                // wrappers shadow any distro-shipped libs.
                put("LD_LIBRARY_PATH",
                    "${LibhybrisInstallProvider.GUEST_GL_SHIMS_DIR}:${LibhybrisInstallProvider.GUEST_LIB_DIR}")
                // No HYBRIS_*_DIR overrides needed — `scripts/build-libhybris.sh`
                // configures libhybris with `--prefix=/usr/lib/hybris
                // --libdir=/usr/lib/hybris`, so the PKGLIBDIR + LINKER_PLUGIN_DIR
                // macros baked into the .so files by the autotools build (see
                // deps/libhybris/hybris/{egl/ws.c, vulkan/ws.c, common/hooks.c})
                // already point at where [LibhybrisInstallProvider] copies the
                // plugin tree. Build-time bake > per-entry env override.
                put("HYBRIS_EGLPLATFORM", "wayland")
                // GTK uses libhybris GLES (→ AHB) instead of falling back to
                // its software/cairo path (→ wl_shm, magenta-tinted). See
                // notes/firefox.md "Why GDK_GL=gles:always". Libhybris-only —
                // under gfxstream we don't have a working GL path yet, so
                // GDK auto-pick / software fallback is the right behaviour.
                put("GDK_GL", "gles:always")
            }
            GraphicsBackend.CPU -> {
                // Software-only path: don't put libhybris on
                // LD_LIBRARY_PATH (so the distro Mesa loads), don't
                // pin a Vulkan ICD (so the distro vulkan-icd-loader
                // auto-picks lavapipe from /usr/share/vulkan/icd.d/
                // if vulkan-swrast is installed). Force Mesa onto
                // llvmpipe regardless of any DRI device that might
                // otherwise be probed. No GDK_GL override — let GTK
                // fall through to its default renderer on llvmpipe.
                put("LIBGL_ALWAYS_SOFTWARE", "1")
                put("GALLIUM_DRIVER", "llvmpipe")
            }
            GraphicsBackend.GFXSTREAM -> {
                // gfxstream path: distro vulkan-icd-loader is the loader
                // (no libhybris libvulkan.so.1 shadowing it), pinned to
                // Mesa's gfxstream-vk ICD. VIRTGPU_KUMQUAT=1 makes Mesa's
                // gfxstream-vk select the userspace kumquat socket
                // transport instead of /dev/dri/cardN, and our Mesa patch
                // (deps/mesa-patches/mesa/03-kumquat-socket-env-override.patch)
                // honours VIRTGPU_KUMQUAT_GPU_SOCKET so the client dials
                // the compositor-process kumquat thread at the path the
                // host-side share bind exposes — `/usr/share/tawc/...`,
                // not `/tmp/...`. See notes/gfxstream-bridge.md.
                put("VK_ICD_FILENAMES", BridgeInstallProvider.GUEST_ICD_PATH)
                put("VIRTGPU_KUMQUAT", "1")
                put("VIRTGPU_KUMQUAT_GPU_SOCKET", "/usr/share/tawc/kumquat-gpu-0")
            }
        }
        put("DISPLAY", ":0")
        // SDL2 prefers X11 when DISPLAY is set, but our Xwayland is
        // GLAMOR-disabled — SDL apps that probe X11 die on createWindow.
        // Force Wayland-first.
        put("SDL_VIDEODRIVER", "wayland,x11")
        if (method == Method.PROOT) {
            put("MOZ_DISABLE_CONTENT_SANDBOX", "1")
            put("MOZ_DISABLE_GPU_SANDBOX", "1")
            put("MOZ_DISABLE_RDD_SANDBOX", "1")
            put("MOZ_DISABLE_SOCKET_PROCESS_SANDBOX", "1")
            put("MOZ_DISABLE_UTILITY_SANDBOX", "1")
            put("MOZ_DISABLE_GMP_SANDBOX", "1")
            put("MOZ_DISABLE_VR_SANDBOX", "1")
        }
    }

    /**
     * `/usr/bin/env -i -C /root KEY=VAL …` argv prefix. Each entry is
     * one argv element so callers don't have to worry about quoting
     * values that contain spaces. Dest is the in-rootfs `/usr/bin/env`
     * (resolved by tawcroot/proot path translation, or by the kernel
     * after chroot).
     *
     * `-C /root` chdir's to root's home before exec'ing the shell, so
     * an interactive `bash -l` lands in `/root` instead of wherever the
     * host-side cwd happened to translate to (`/tmp` for tawcroot's
     * `$rootfs/tmp` cwd; `/` for chroot's post-chroot cwd). Matches
     * `HOME=/root` set above. Requires GNU env (coreutils ≥ 9.0); both
     * Arch and Void ship 9.x.
     */
    fun envArgv(method: Method): List<String> = envArgv(method, Settings.graphicsBackend)

    fun envArgv(method: Method, backend: GraphicsBackend): List<String> {
        val out = ArrayList<String>(4 + 16)
        out += "/usr/bin/env"
        out += "-i"
        out += "-C"
        out += "/root"
        for ((k, v) in build(method, backend)) out += "$k=$v"
        return out
    }
}
