package me.phie.tawc.install

import android.os.Build

/**
 * Bind-mount setup / teardown for a chroot installation.
 *
 * Magisk's `su` runs each invocation in a private mount namespace, so
 * any bind mounts done in one `su` call are torn down when that call
 * exits — mount + chroot must therefore live in the same `su -c "..."`
 * shell. [mountScript] returns the snippet that performs all bind
 * mounts; [ChrootMethod.startInside] embeds it ahead of the
 * `exec setsid chroot …` line.
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
        val tawcShare = "/data/data/me.phie.tawc/share"
        val guestShare = TawcrootMethod.GUEST_TAWC_SHARE_DIR

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
            # Expose JUST the compositor's `share/` subdir at
            # /usr/share/tawc inside the chroot — the wayland socket
            # and Xwayland's xtmp dir live there. Deliberately not the
            # whole <appData> tree (which would expose libhybris's
            # asset extract, the proot scratch dir, and everything
            # else under <filesDir> to in-rootfs writes — see
            # notes/installation.md "/usr/share/tawc"). RootfsEnv
            # points WAYLAND_DISPLAY at the in-rootfs path; no
            # /tmp/wayland-0 symlink needed. mkdir the source first
            # so the bind succeeds even on a fresh device before the
            # compositor has run.
            mkdir -p "$tawcShare"
            mkdir -p "${'$'}ROOTFS$guestShare"
            mount_if_needed "$tawcShare" "${'$'}ROOTFS$guestShare"
            """.trimIndent()
        )
        sb.appendLine(
            """
            # XWayland: the bionic-built Xwayland binary on the Android
            # side opens its X11 listening socket at
            # <appData>/share/xtmp/.X11-unix/X<n> (because Android has
            # no /tmp). Bind that into the chroot so X clients see it
            # at the standard /tmp/.X11-unix path. libxcb hardcodes
            # /tmp/.X11-unix/X<N> for the `:N` form of DISPLAY, so
            # we can't just expose it via /usr/share/tawc. mkdir the
            # source before binding so the mount succeeds even before
            # the compositor has launched Xwayland (install steps,
            # tests).
            mkdir -p "$tawcShare/xtmp/.X11-unix"
            mkdir -p "${'$'}ROOTFS/tmp/.X11-unix"
            mount_if_needed "$tawcShare/xtmp/.X11-unix" "${'$'}ROOTFS/tmp/.X11-unix"
            """.trimIndent()
        )
        return sb.toString()
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
