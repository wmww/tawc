package me.phie.tawc.install

import android.content.Context
import java.io.File

/**
 * Outcome of an [InstallationMethod.runOutside] / `runInside` call —
 * mirrors [Su.Result] so callers don't care which method produced it.
 */
data class MethodResult(val exitCode: Int, val output: String) {
    val ok: Boolean get() = exitCode == 0
}

/**
 * Strategy for setting up and entering an installed Linux rootfs.
 *
 * Two implementations today:
 *
 *   - [ChrootMethod] — real `chroot(2)` plus `mount(2)` bind mounts.
 *     Requires Magisk's `su`. The rootfs is owned by uid 0 on disk; in-
 *     rootfs processes appear as their declared uid via the kernel's
 *     normal credential mechanism.
 *
 *   - [ProotMethod] — ptrace-based fake chroot via the vendored proot
 *     binary. No privilege required. The rootfs is owned by the app uid
 *     on disk; proot lies to in-rootfs processes about uid/path so they
 *     see uid 0 and a `/` rooted at our rootfs.
 *
 * The rest of the install code (Installer, Distro implementations,
 * RootfsCleaner) is method-agnostic — anywhere it would have called
 * `Su.run(...)` or chroot'd a script directly it now goes through
 * [runOutside] / [runInside] on this interface.
 *
 * Method choice is recorded in [Installation.method] at install time,
 * carried in `metadata.json` for the lifetime of the install, and used
 * for every operation against that rootfs (configure, package install,
 * uninstall). Re-running [enterScript] each operation keeps the
 * on-disk launcher script in sync with the Kotlin source.
 *
 * See `notes/installation.md` for the install-pipeline overview and
 * `notes/architecture.md` for where each method fits relative to the
 * compositor / SELinux story.
 */
interface InstallationMethod {
    /** Stable id stored in `Installation.method`. */
    val key: String

    /** Human-readable label for UI. */
    val displayName: String

    /**
     * True if this method needs Magisk `su` to function. Used by the
     * UI to gate install with a clear error rather than letting the
     * pipeline fail mid-flight.
     */
    val requiresRoot: Boolean

    /**
     * Quick is-this-runnable check for the host environment. For
     * [ChrootMethod] this calls [Su.rootAvailable]; for [ProotMethod]
     * it verifies the vendored binary is present and executable.
     */
    fun isAvailable(context: Context): Boolean

    /**
     * Run a shell script with the privileges needed to manipulate
     * [rootfs] from outside (e.g. write `/etc/...` files, mass-delete
     * cruft trees). The script should reference the rootfs via the
     * host-side path it gets handed; both methods expose that path
     * directly because in chroot mode the rootfs is uid 0-owned and in
     * proot mode it's app-uid-owned, and either way our shell can
     * access it without any path translation.
     *
     * For [ChrootMethod] this is `su` (root, app's mount namespace).
     * For [ProotMethod] it's a plain `/system/bin/sh` running as the
     * app uid.
     */
    fun runOutside(
        script: String,
        onLine: ((String) -> Unit)? = null,
    ): MethodResult

    /**
     * Run [command] *inside* [rootfs] (a `bash -lc` shell rooted at
     * `rootfs`). For [ChrootMethod] this routes through the on-disk
     * `enter.sh` (mount + chroot) under `su`; for [ProotMethod] it
     * exec's the vendored proot binary with `-r <rootfs> -0` plus the
     * standard binds (`/dev`, `/proc`, `/sys`, the app data dir for
     * the Wayland socket).
     *
     * `bash -lc` so the profile.d entries run and the chroot's PATH,
     * `LD_LIBRARY_PATH`, `WAYLAND_DISPLAY` env get set.
     */
    fun runInside(
        rootfs: String,
        command: String,
        onLine: ((String) -> Unit)? = null,
    ): MethodResult

    /**
     * Tar-extract [tarball] into [rootfs]. The implementations diverge:
     * [ChrootMethod] runs toybox `tar` via `su` (files land with the
     * uids the archive recorded). [ProotMethod] uses a pure-Kotlin
     * [ProotArchiveExtractor] — proot itself COULD wrap `tar` with
     * `-0` to fake root, but toybox honours recorded dir modes (0500
     * in the Arch bootstrap's `cadir`) immediately and blocks any
     * subsequent app-uid write inside them, with no `--no-same-perms`
     * knob to opt out. The Kotlin extractor defers dir-mode application
     * until after children are written. [tempFifo] is used only by the
     * chroot path (zstd → kernel FIFO → toybox tar); the Kotlin
     * extractor pipes via zstd-jni in-process. [stripPrefix] flattens
     * a single top-level dir if the tarball wraps everything in one
     * (as Arch x86_64's `root.x86_64/` does).
     */
    fun extractBootstrap(
        tarball: File,
        rootfs: String,
        format: BootstrapFormat,
        stripPrefix: String?,
        tempFifo: File,
        onLine: (String) -> Unit,
    )

    /**
     * Wipe [installDir] (rootfs + metadata + enter.sh + anything else
     * we put there). The chroot impl has the more involved sequence
     * (kill chroot procs, strict unmount, find -delete via `su`);
     * proot's wipe is a plain recursive delete since the dir is
     * app-uid-owned and there are no global mounts to tear down.
     *
     * Throws on failure — the caller (InstallationService) parks the
     * slot in `FAILED` state if the dir survives.
     */
    fun wipe(installDir: File, log: (String) -> Unit)

    /**
     * Render the contents of `enter.sh` for an installation rooted at
     * [rootfs]. Both in-app callers (via this method's [runInside])
     * and host-side `client/tawc-chroot-run` exec this exact script,
     * so the launcher logic only lives here.
     *
     * For [ChrootMethod] the script is a `#!/system/bin/sh` that
     * assumes its caller is uid 0 — it does the bind mounts then
     * `exec chroot`. For [ProotMethod] the script runs as app uid and
     * just exec's the proot binary with the right flags. The host
     * launcher script branches on the install's `method` to decide
     * whether to wrap in `su -c` or `run-as <pkg>`.
     */
    fun enterScript(context: Context, rootfs: String): String

    companion object {
        /** Maps a stored `Installation.method` string to the implementation. */
        fun forKey(context: Context, key: String): InstallationMethod? = when (key) {
            ChrootMethod.KEY -> ChrootMethod
            ProotMethod.KEY -> ProotMethod(context)
            TawcrootMethod.KEY -> TawcrootMethod(context)
            else -> null
        }

        /**
         * Auto-pick a method for a fresh install. Prefer chroot when
         * `su` is available (faster runtime, integrates with
         * libhybris) and fall back to proot when it isn't. Callers
         * may override at install time via the UI / `--es method` CLI.
         */
        fun defaultForHost(context: Context): InstallationMethod =
            if (Su.rootAvailable()) ChrootMethod else ProotMethod(context)
    }
}
