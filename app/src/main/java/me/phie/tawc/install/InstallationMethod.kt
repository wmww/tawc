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
     * Start a subprocess running [command] inside [rootfs] (a
     * `bash -lc` shell rooted there) and return the started [Process]
     * for the caller to stream stdio from. Caller is responsible for
     * `waitFor()` and reaping.
     *
     * This is the single entry point for "enter the chroot" — the
     * broker's RUNINSIDE handler and the in-app [runInside] helper
     * both route here. Chroot-session invariant
     * (notes/chroot-sessions.md) is upheld in here, in one place.
     *
     * `command == null` means interactive `bash -l` — drops into a
     * login shell with no command. Useful for `tawc-chroot-run.sh`
     * with no args.
     *
     * Method-specific notes:
     *   - [ChrootMethod] needs `su` (CAP_SYS_CHROOT). The bind-mount
     *     + chroot script is piped to `su` via stdin, which means the
     *     in-rootfs bash doesn't see the caller's stdin. The other
     *     methods don't have this limitation.
     *   - [TawcrootMethod] / [ProotMethod] run as the app uid via
     *     `ProcessBuilder.start()` with `setsid` prepended.
     *
     * Pre-setup (mkdirs for bind targets, refresh of
     * `/etc/profile.d/01-tawc.sh`) happens in Kotlin here; nothing
     * needs to be on disk between calls.
     */
    fun startInside(rootfs: String, command: String?): Process

    /**
     * Convenience wrapper around [startInside] for in-process callers
     * that just want the exit code and combined output (e.g. the
     * Installer pipeline's `pacman -Syyu` step). Default impl streams
     * stdout/stderr line-by-line through [onLine] and waits for the
     * process to exit.
     *
     * `bash -lc` so the profile.d entries run and the chroot's PATH,
     * `LD_LIBRARY_PATH`, `WAYLAND_DISPLAY` env get set.
     */
    fun runInside(
        rootfs: String,
        command: String,
        onLine: ((String) -> Unit)? = null,
    ): MethodResult = MethodRunHelper.runInside(this, rootfs, command, onLine)

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

    companion object {
        /**
         * Map a stored `Installation.method` string to the
         * implementation, or null if the method is unknown OR disabled
         * in this build (see [EnabledMethods]). A null on the install
         * path is rejected up front by the service gate; on the
         * uninstall path the caller falls back to [defaultForHost] so
         * a legacy slot recorded against a now-disabled method is
         * still cleanable.
         */
        fun forKey(context: Context, key: String): InstallationMethod? = when (key) {
            ChrootMethod.KEY -> if (EnabledMethods.chroot) ChrootMethod else null
            ProotMethod.KEY -> if (EnabledMethods.proot) ProotMethod(context) else null
            TawcrootMethod.KEY -> if (EnabledMethods.tawcroot) TawcrootMethod(context) else null
            else -> null
        }

        /**
         * Auto-pick a method for a fresh install. tawcroot first
         * whenever it's enabled (the default and only officially
         * supported method); otherwise the next enabled method in
         * preference order: proot, then chroot (only if `su` works).
         * Callers may override at install time via the UI /
         * `--es method` CLI.
         */
        fun defaultForHost(context: Context): InstallationMethod {
            if (EnabledMethods.tawcroot) return TawcrootMethod(context)
            if (EnabledMethods.proot) return ProotMethod(context)
            if (EnabledMethods.chroot && Su.rootAvailable()) return ChrootMethod
            error("no install methods enabled — check BuildConfig.METHOD_*_ENABLED")
        }
    }
}
