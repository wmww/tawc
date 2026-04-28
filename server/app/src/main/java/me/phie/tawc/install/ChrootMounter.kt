package me.phie.tawc.install

import android.os.Build

/**
 * Bind-mount setup / teardown for a chroot installation, plus the
 * canonical `enter.sh` script that combines mount-setup + chroot-exec
 * into one shell.
 *
 * Magisk's `su` runs each invocation in a private mount namespace, so
 * any bind mounts done in one `su` call are torn down when that call
 * exits — mount + chroot must therefore live in the same `su -c "..."`
 * shell. To keep that logic in one place across both in-app callers
 * ([ChrootMethod.runInside]) and host-side tooling
 * (`client/tawc-chroot-run` over `adb shell su`), we render an
 * `enter.sh` shell script into `<installation-dir>/enter.sh` at install
 * time and then everyone invokes the same file. [enterScript] returns
 * its body; [mountScript] is a lower-level building block exposed for
 * the script generator.
 *
 * [unmount] still exists for the case where a previous run somehow
 * leaked mounts into the global namespace (e.g. via `su --mount-master`)
 * before we delete the rootfs.
 *
 * This is the only piece that knows about the difference between an
 * emulator and a real device — emulators skip the libhybris-only mounts
 * (`vendor`, `system`, `system_ext`, `apex`, `binderfs`, `linkerconfig`)
 * because libhybris doesn't run on the gfxstream GPU stack.
 */
object ChrootMounter {

    /** True if we look like we're running inside an Android emulator. */
    private val isEmulator: Boolean by lazy {
        Build.HARDWARE.contains("ranchu") ||
            Build.FINGERPRINT.startsWith("generic") ||
            Build.PRODUCT.startsWith("sdk_") ||
            Build.MODEL.contains("Emulator") ||
            Build.MODEL.contains("Android SDK")
    }

    /**
     * Shell snippet that performs all bind mounts for the chroot rooted
     * at [rootfs]. Aborts (with `exit 1`) if `$rootfs/usr` is missing.
     * The snippet assumes `set -eu` has been prepended (Su.run does that)
     * and is safe to source from a longer wrapper script — it leaves
     * `ROOTFS`, `MOUNTS`, `is_mounted`, and `mount_if_needed` defined for
     * the rest of the script to use.
     */
    fun mountScript(rootfs: String): String {
        val emulator = isEmulator
        val tawcData = "/data/data/me.phie.tawc"

        val sb = StringBuilder()
        sb.appendLine("ROOTFS='$rootfs'")
        sb.appendLine(
            """
            if [ ! -d "${'$'}ROOTFS/usr" ]; then
                echo "ERROR: rootfs not found: ${'$'}ROOTFS" >&2
                exit 1
            fi

            mkdir -p "${'$'}ROOTFS/dev" "${'$'}ROOTFS/dev/pts" "${'$'}ROOTFS/proc" "${'$'}ROOTFS/sys"

            MOUNTS=${'$'}(cat /proc/mounts)
            is_mounted() { echo "${'$'}MOUNTS" | grep -q " ${'$'}1 "; }
            mount_if_needed() {
                src="${'$'}1"; dst="${'$'}2"
                is_mounted "${'$'}dst" || mount -o bind,rslave "${'$'}src" "${'$'}dst"
            }

            mount_if_needed /dev      "${'$'}ROOTFS/dev"
            mount_if_needed /dev/pts  "${'$'}ROOTFS/dev/pts"
            mount_if_needed /proc     "${'$'}ROOTFS/proc"
            mount_if_needed /sys      "${'$'}ROOTFS/sys"
            """.trimIndent()
        )

        if (!emulator) {
            sb.appendLine(
                """
                mkdir -p "${'$'}ROOTFS/dev/binderfs" "${'$'}ROOTFS/vendor" \
                         "${'$'}ROOTFS/system" "${'$'}ROOTFS/system_ext" \
                         "${'$'}ROOTFS/linkerconfig" "${'$'}ROOTFS/apex"

                mount_if_needed /dev/binderfs "${'$'}ROOTFS/dev/binderfs"
                mount_if_needed /vendor       "${'$'}ROOTFS/vendor"
                mount_if_needed /system       "${'$'}ROOTFS/system"
                mount_if_needed /system_ext   "${'$'}ROOTFS/system_ext"
                mount_if_needed /linkerconfig "${'$'}ROOTFS/linkerconfig"

                # /apex is recursive bind: each APEX is its own loop mount.
                is_mounted "${'$'}ROOTFS/apex" || mount -o rbind,rslave /apex "${'$'}ROOTFS/apex"

                # Belt-and-braces: com.android.runtime sometimes doesn't
                # propagate (private mounts). libhybris needs bionic libs.
                if [ -d /apex/com.android.runtime/lib64 ] && \
                   [ ! -d "${'$'}ROOTFS/apex/com.android.runtime/lib64" ]; then
                    mount --rbind /apex/com.android.runtime "${'$'}ROOTFS/apex/com.android.runtime"
                fi

                # Some vendor libbinder.so is older than system's; overlay system's.
                if [ -f /system/lib64/libbinder.so ] && \
                   [ -f "${'$'}ROOTFS/vendor/lib64/libbinder.so" ]; then
                    is_mounted "${'$'}ROOTFS/vendor/lib64/libbinder.so" || \
                        mount --bind /system/lib64/libbinder.so "${'$'}ROOTFS/vendor/lib64/libbinder.so" 2>/dev/null || true
                fi

                # SELinux transition for chroot-client memfds (no-op on
                # emulators where magiskpolicy isn't installed; setenforce 0
                # is used there instead, see notes/emulator.md).
                magiskpolicy --live "type_transition magisk tmpfs file appdomain_tmpfs" 2>/dev/null || true
                """.trimIndent()
            )
        }

        sb.appendLine(
            """
            # Mount the compositor's app data dir at the same path inside
            # the chroot so /data/data/me.phie.tawc/wayland-0 (the Wayland
            # socket) is reachable. The chroot's profile.d/01-tawc.sh
            # symlinks it to /tmp/wayland-0.
            #
            # Yes, this creates a recursion (the rootfs contains itself
            # via .../distros/<id>/rootfs/data/data/...). It's
            # benign because no tool recursively walks through it.
            if [ -d "$tawcData" ]; then
                mkdir -p "${'$'}ROOTFS$tawcData"
                mount_if_needed "$tawcData" "${'$'}ROOTFS$tawcData"
            fi
            """.trimIndent()
        )
        // Refresh profile.d/01-tawc.sh on every chroot entry so changes to
        // the Wayland env (LD_LIBRARY_PATH, HYBRIS_EGLPLATFORM, …) take
        // effect without reinstalling. Cheap (<1ms). 00-path.sh is
        // install-time-only because it's identical for every install.
        sb.appendLine(
            """
            mkdir -p "${'$'}ROOTFS/etc/profile.d"
            cat > "${'$'}ROOTFS/etc/profile.d/01-tawc.sh" <<'TAWC_PROF_EOF'
            # tawc Wayland compositor environment (refreshed each chroot entry)
            export WAYLAND_DISPLAY=wayland-0
            export XDG_RUNTIME_DIR=/tmp
            export LD_LIBRARY_PATH=/usr/local/lib/gl-shims:/usr/local/lib
            export HYBRIS_EGLPLATFORM=wayland
            ln -sf /data/data/me.phie.tawc/wayland-0 /tmp/wayland-0 2>/dev/null
            TAWC_PROF_EOF
            chmod 644 "${'$'}ROOTFS/etc/profile.d/01-tawc.sh"
            """.trimIndent()
        )
        return sb.toString()
    }

    /**
     * Body of the `enter.sh` shell script that lives at
     * `<installation-dir>/enter.sh` after install. Self-contained
     * (rootfs path is baked in, no required env), POSIX/mksh-compatible
     * so it works on the bare Android shell as well as bash. Takes the
     * chroot command as base64 in `$1` to dodge quoting hell across the
     * `adb shell → su -c → chroot bash` chain. Empty `$1` (or no args)
     * drops into an interactive `bash -l` inside the chroot.
     *
     * Both in-app callers (via [ChrootMethod.runInside]) and host-side
     * `client/tawc-chroot-run` invoke this exact script; the mount
     * logic only lives here.
     */
    fun enterScript(rootfs: String): String = buildString {
        appendLine("#!/system/bin/sh")
        appendLine("# Auto-generated by ChrootMounter.enterScript. Do not edit by hand —")
        appendLine("# rewritten on every install / chroot entry.")
        appendLine("set -eu")
        appendLine(mountScript(rootfs))
        appendLine(
            """
            if [ ${'$'}# -gt 0 ] && [ -n "${'$'}1" ]; then
                # `printf %s | base64 -d` so the encoded payload can contain any bytes
                # (including newlines) without quoting through this script.
                CMD=${'$'}(printf %s "${'$'}1" | base64 -d)
                exec chroot "${'$'}ROOTFS" /bin/bash -lc "${'$'}CMD"
            else
                exec chroot "${'$'}ROOTFS" /bin/bash -l
            fi
            """.trimIndent()
        )
    }

    /**
     * Defensive cleanup: unmount anything that's currently bind-mounted
     * under [rootfs] in the global namespace and verify the dir is mount-
     * free. Called before deleting the rootfs so an `rm -rf` can never
     * traverse a live bind mount into real system files (deleting the
     * host's /dev/socket etc.).
     *
     * Runs via `su -mm` (Magisk's mount-master mode) so `umount` actually
     * affects the global mount table. The default `su` we get when
     * `ProcessBuilder("su")` is invoked from an Android app sits in a
     * per-app mount namespace — `umount` there silently succeeds without
     * touching PID 1's view, which leaves the leaked bind mounts in
     * place and the uninstall step then refuses to delete the rootfs.
     *
     * The path Kotlin gives us (`/data/user/0/<pkg>/...`) is a symlink
     * target; /proc/mounts reports the canonical `/data/data/<pkg>/...`
     * form. We `realpath` once before matching so both forms work, and
     * use a strict prefix check (not regex) so paths with dots don't
     * over-match. Each leaked mount is also exposed at the
     * `/data/user/0/...` and `/data_mirror/data_ce/null/0/...` paths
     * via Android's storage propagation; unmounting the canonical
     * `/data/data/...` form propagates the unmount through all three.
     */
    fun unmount(rootfs: String): Su.Result {
        val script = """
            CANON=${'$'}(realpath '$rootfs' 2>/dev/null || echo '$rootfs')
            list_mounts() {
                awk -v r="${'$'}CANON" '${'$'}2 == r || index(${'$'}2, r"/") == 1 {print ${'$'}2}' /proc/mounts
            }
            for m in ${'$'}(list_mounts | sort -r); do
                # Try a regular umount silently; on failure fall back to a
                # lazy umount and let its stderr through so the cause shows
                # up in the log if even lazy fails.
                umount "${'$'}m" 2>/dev/null && continue
                umount -l "${'$'}m" || true
            done
            remaining=${'$'}(list_mounts | wc -l)
            if [ "${'$'}remaining" -gt 0 ]; then
                echo "ERROR: ${'$'}remaining mount(s) still active under ${'$'}CANON:" >&2
                list_mounts >&2
                exit 1
            fi
            echo OK
        """.trimIndent()
        return Su.run(script, mountMaster = true)
    }
}
