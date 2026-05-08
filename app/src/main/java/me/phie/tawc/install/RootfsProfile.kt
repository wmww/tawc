package me.phie.tawc.install

/**
 * `/etc/profile.d/01-tawc.sh` body — sourced by login bash inside the
 * rootfs to set Wayland/GL env and surface X11 sockets at canonical
 * paths.
 *
 * Refreshed on every entry by each install method
 * ([TawcrootMethod.startInside], [ProotMethod.startInside],
 * [ChrootMounter.mountScript]) so changes here pick up without
 * reinstalling.
 *
 * Two divergences across methods:
 *
 *   - **PROOT** adds `MOZ_DISABLE_*_SANDBOX`. Firefox's per-subprocess
 *     sandbox setup SIGSEGVs under proot's ptrace tracer; tawcroot and
 *     chroot have no tracer and the sandbox initialises cleanly, so
 *     applying these vars there would just weaken security for no gain.
 *   - **CHROOT** skips the X11 socket symlinks. `ChrootMounter`
 *     bind-mounts `/tmp/.X11-unix` directly, and a `ln -sfn` over a
 *     mountpoint would fail.
 */
internal object RootfsProfile {
    enum class Method { TAWCROOT, PROOT, CHROOT }

    fun build(method: Method): String = buildString {
        appendLine("# tawc Wayland compositor environment (refreshed on every entry)")
        // Absolute path rather than bare `wayland-0` + /tmp symlink:
        // proot's symlink-target resolution misses the
        // `-b /data/data/<pkg>` rewrite, and there's no reason for the
        // chroot/tawcroot paths to do anything different. The
        // /tmp/wayland-0 symlink below stays for any consumer that
        // relies on it.
        appendLine("export WAYLAND_DISPLAY=/data/data/me.phie.tawc/wayland-0")
        appendLine("export XDG_RUNTIME_DIR=/tmp")
        appendLine("export LD_LIBRARY_PATH=/usr/local/lib/gl-shims:/usr/local/lib")
        appendLine("export HYBRIS_EGLPLATFORM=wayland")
        appendLine("export DISPLAY=:0")
        // SDL2 prefers X11 when DISPLAY is set, but our Xwayland is
        // GLAMOR-disabled — SDL apps that probe X11 die on createWindow.
        // Force Wayland-first.
        appendLine("export SDL_VIDEODRIVER=wayland,x11")
        // GTK uses libhybris GLES (→ AHB) instead of falling back to
        // its software/cairo path (→ wl_shm, magenta-tinted). See
        // notes/firefox.md "Why GDK_GL=gles:always".
        appendLine("export GDK_GL=gles:always")
        appendLine("ln -sf /data/data/me.phie.tawc/wayland-0 /tmp/wayland-0 2>/dev/null")
        if (method != Method.CHROOT) {
            // X11 sockets land in /data/data/me.phie.tawc/xtmp/ on the
            // host (Android has no /tmp); surface them at /tmp/.X11-unix.
            appendLine("ln -sfn /data/data/me.phie.tawc/xtmp/.X11-unix /tmp/.X11-unix 2>/dev/null")
            appendLine("for lock in /data/data/me.phie.tawc/xtmp/.X*-lock; do")
            appendLine("    [ -f \"\$lock\" ] || continue")
            appendLine("    ln -sf \"\$lock\" \"/tmp/\${lock##*/}\" 2>/dev/null")
            appendLine("done")
        }
        if (method == Method.PROOT) {
            appendLine("export MOZ_DISABLE_CONTENT_SANDBOX=1")
            appendLine("export MOZ_DISABLE_GPU_SANDBOX=1")
            appendLine("export MOZ_DISABLE_RDD_SANDBOX=1")
            appendLine("export MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1")
            appendLine("export MOZ_DISABLE_UTILITY_SANDBOX=1")
            appendLine("export MOZ_DISABLE_GMP_SANDBOX=1")
            appendLine("export MOZ_DISABLE_VR_SANDBOX=1")
        }
    }
}
