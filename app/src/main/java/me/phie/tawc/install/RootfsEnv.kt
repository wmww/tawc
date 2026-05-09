package me.phie.tawc.install

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
 */
internal object RootfsEnv {
    enum class Method { TAWCROOT, PROOT, CHROOT }

    fun build(method: Method): Map<String, String> = buildMap {
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
        put("DISPLAY", ":0")
        // SDL2 prefers X11 when DISPLAY is set, but our Xwayland is
        // GLAMOR-disabled — SDL apps that probe X11 die on createWindow.
        // Force Wayland-first.
        put("SDL_VIDEODRIVER", "wayland,x11")
        // GTK uses libhybris GLES (→ AHB) instead of falling back to
        // its software/cairo path (→ wl_shm, magenta-tinted). See
        // notes/firefox.md "Why GDK_GL=gles:always".
        put("GDK_GL", "gles:always")
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
     * `/usr/bin/env -i KEY=VAL …` argv prefix. Each entry is one argv
     * element so callers don't have to worry about quoting values that
     * contain spaces. Dest is the in-rootfs `/usr/bin/env` (resolved by
     * tawcroot/proot path translation, or by the kernel after chroot).
     */
    fun envArgv(method: Method): List<String> {
        val out = ArrayList<String>(2 + 16)
        out += "/usr/bin/env"
        out += "-i"
        for ((k, v) in build(method)) out += "$k=$v"
        return out
    }
}
