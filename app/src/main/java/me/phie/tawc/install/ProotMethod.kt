package me.phie.tawc.install

import android.content.Context
import android.util.Log
import java.io.File
import java.io.IOException

/**
 * Rootless implementation of [InstallationMethod] using the vendored
 * `proot` binary (built by `scripts/build-proot.sh`, shipped in the APK
 * via `jniLibs/<abi>/libproot.so`).
 *
 * Unlike [ChrootMethod], proot is a userspace ptrace-based fake chroot
 * — it intercepts each tracee's syscalls and rewrites paths/uids in
 * user space rather than asking the kernel to do it. That removes the
 * `CAP_SYS_CHROOT` and `CAP_SYS_ADMIN` requirements at the cost of
 * per-syscall ptrace overhead. The on-disk rootfs layout is identical
 * to chroot's, just owned by the app uid instead of root — proot lies
 * about ownership when in-rootfs processes call `stat(2)`.
 *
 * Constructed per-context so we can resolve the binary path through
 * `applicationInfo.nativeLibraryDir`. We don't cache as a singleton
 * because contexts (configuration changes, alternate users) can shift
 * the path.
 */
class ProotMethod(context: Context) : InstallationMethod {
    /** Absolute path to the vendored proot binary on disk. */
    val prootBin: String =
        File(context.applicationInfo.nativeLibraryDir, "libproot.so").absolutePath

    /**
     * Pre-extracted loader stub. proot would otherwise write its
     * loader to `$PROOT_TMP_DIR/prooted-XXXXXX` and `execve` it from
     * there — which Android 10+ blocks for apps targeting API 29+
     * (apps cannot execve files in their own home directory; the
     * kernel raises SIGSYS via seccomp). Pointing `PROOT_LOADER` at
     * the loader we shipped in `nativeLibraryDir` (which has
     * `apk_data_file` SELinux context, where exec is allowed) skips
     * that extract-and-exec step entirely.
     */
    val prootLoader: String =
        File(context.applicationInfo.nativeLibraryDir, "libproot-loader.so").absolutePath

    /**
     * Scratch dir proot uses for any other small temp files. Default
     * is `/tmp`, which on Android isn't writable for the app uid; we
     * point at the app's own cacheDir so it follows the app lifecycle
     * and gets cleared by Android's "clear cache" mechanism alongside
     * everything else.
     */
    private val prootTmpDir: String = File(context.cacheDir, "proot-tmp").absolutePath

    /**
     * Host-side directory bound at `/dev/shm` inside the rootfs. proot
     * passes `/dev` through unchanged from the host, where Android has
     * no `/dev/shm` and no policy that would let app uid create one;
     * Firefox's parent process uses POSIX `shm_open(3)` for its main
     * IPC shared-memory segment and `MOZ_RELEASE_ASSERT`s on success,
     * so an unbacked `/dev/shm` is a hard crash. Pointing this at
     * `filesDir` (rather than `cacheDir`) keeps Android from purging
     * the backing file under storage pressure mid-session, which would
     * SIGBUS any process holding the segment open.
     */
    private val devShmDir: String = File(context.filesDir, "proot-dev-shm").absolutePath

    override val key: String = KEY
    override val displayName: String = "proot (rootless)"
    override val requiresRoot: Boolean = false

    /**
     * proot is shipped in the APK; the only failure mode is the build
     * script not having been run before the APK was assembled. Treat a
     * missing-or-non-executable binary as `unavailable`.
     */
    override fun isAvailable(context: Context): Boolean {
        val f = File(prootBin)
        return f.exists() && f.canExecute()
    }

    /**
     * Run a host-side shell script as the app uid. No privilege
     * escalation — under proot the rootfs is app-uid-owned, so plain
     * file writes and recursive deletes against `<rootfs>/...` work
     * without any `su`. The shell is the system one (`/system/bin/sh`,
     * mksh) for parity with what `Su.run` ends up invoking; nothing
     * here needs bash.
     */
    override fun runOutside(
        script: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult =
        // `set -eu` to match Su.run's contract — the chroot path
        // implicitly aborts on first failure, so callers writing
        // multi-step scripts (e.g. ArchPacmanCommon.configure) can
        // expect the same here. Don't add this to the runInside or
        // wipe paths; both have their own structured exit handling
        // that fights with -e.
        runShell(listOf("/system/bin/sh"), "set -eu\n$script", onLine)

    /**
     * Run [command] inside a proot-fake-chroot at [rootfs]. The proot
     * args mirror the canonical "Termux proot-distro" shape:
     *
     *   - `-r <rootfs>`         — chroot here (path-rewrite root).
     *   - `-0`                  — emulate uid 0 for the tracee tree.
     *   - `-b /dev`, `-b /proc`, `-b /sys` — pass kernel-managed
     *     filesystems through; proot doesn't fake these.
     *   - `-b <appData>/share:/usr/share/tawc` — exposes JUST the
     *     compositor's `share/` subdir (wayland socket, Xwayland
     *     xtmp) at /usr/share/tawc inside the rootfs. Deliberately
     *     not the whole <appData> tree.
     *   - `-b <appData>/share/xtmp/.X11-unix:/tmp/.X11-unix` —
     *     surfaces Xwayland's listening socket at the canonical X11
     *     path. Asymmetric bind (libxcb hardcodes /tmp/.X11-unix for
     *     `:N` $DISPLAY).
     *   - `--link2symlink`      — turn hardlink calls into symlink
     *     calls. Pacman occasionally hardlinks across mounts; proot
     *     can't always satisfy that on Android.
     *   - `--kill-on-exit`      — SIGKILL the descendant tree if the
     *     proot leader dies. Without this, a crashed `pacman` can
     *     leave `gpg-agent` running (same problem chroot had — see
     *     RootfsCleaner).
     *
     * The in-rootfs bash starts under `/usr/bin/env -i KEY=VAL …` so
     * nothing the host JVM, Android, or proot itself
     * (`PROOT_LOADER`/`PROOT_TMP_DIR`) leaks through — the bash sees
     * exactly [RootfsEnv]'s map. `bash -lc` still runs the distro's
     * /etc/profile + profile.d so locale and package PATH additions
     * still apply.
     */
    override fun startInside(rootfs: String, command: String?): Process {
        // Pre-create the bind targets and proot's scratch dir. proot
        // refuses to bind to a guest path that doesn't exist on disk,
        // so we materialise `<rootfs>/usr/share/tawc` (the wayland
        // socket bind) and the libhybris bind mounts (see
        // [LIBHYBRIS_BIND_DIRS]) before invoking it.
        File(prootTmpDir).mkdirs()
        File(devShmDir).mkdirs()
        File(rootfs, TawcrootMethod.GUEST_TAWC_SHARE_DIR.removePrefix("/")).mkdirs()
        for (dir in LIBHYBRIS_BIND_DIRS) {
            File(rootfs, dir.removePrefix("/")).mkdirs()
        }
        // Source for the X11-socket fake bind plus the wayland socket
        // dir. Compositor mkdirs the X11-unix subdir before launching
        // Xwayland too; recreating here is harmless and lets pre-
        // compositor entries (install steps, tests) bind it without
        // relying on launch order. The bare /share dir is also mkdir'd
        // so proot can satisfy the `-b` source-must-exist check on a
        // fresh device before the compositor has run.
        File(TAWC_SHARE).mkdirs()
        File("$TAWC_SHARE/xtmp/.X11-unix").mkdirs()

        // We invoke proot through `/system/bin/sh -c …` rather than as
        // direct ProcessBuilder argv. Direct exec of the proot binary
        // from app context produces a silent exit-255 (process forks,
        // the loader stub fails to execve_no_trans through the
        // SELinux+seccomp gauntlet); the shell-mediated path works.
        //
        // setsid upholds the rootfs-session invariant (see
        // notes/rootfs-sessions.md): every chroot invocation runs in
        // its own session.
        //
        // The user command is passed as positional arg `$1` to dodge
        // shell-layer quoting — sh -c "<script>" $0 $1 makes $1 = the
        // user command verbatim.
        val invokeArgv =
            prootArgv(rootfs) + RootfsEnv.envArgv(RootfsEnv.Method.PROOT)
        val invokeShell = invokeArgv.joinToString(" ") { shellQuote(it) }
        val script = if (command != null) {
            "exec /system/bin/setsid $invokeShell /bin/bash -lc \"\$1\""
        } else {
            "exec /system/bin/setsid $invokeShell /bin/bash -l"
        }
        val argv = listOf(
            "/system/bin/sh", "-c", script,
            "tawc-proot",                  // $0
            command ?: "",                 // $1 (ignored if no command)
        )
        // PROOT_TMP_DIR & PROOT_LOADER via env on the host-side proot
        // process — cleared at the env -i barrier so they don't leak
        // into the in-rootfs bash. PROOT_TMP_DIR avoids /tmp (not app-
        // writable on Android); PROOT_LOADER points at our vendored
        // loader stub so proot skips the extract-to-tmp+execve dance
        // that API 29+ blocks for app data dirs.
        return ProcessBuilder(argv)
            .also {
                it.environment().clear()
                it.environment()["PROOT_TMP_DIR"] = prootTmpDir
                it.environment()["PROOT_LOADER"] = prootLoader
            }
            .start()
    }

    /** Quote [s] for inclusion in a /system/bin/sh single-quoted arg. */
    private fun shellQuote(s: String): String =
        "'" + s.replace("'", "'\\''") + "'"

    /**
     * Extract the bootstrap in pure Kotlin via [ProotArchiveExtractor].
     * Toybox `tar` (the only tar we have for the chroot path) honours
     * recorded directory modes immediately — and the Arch bootstrap
     * ships `/etc/ca-certificates/extracted/cadir/` with mode 0500. As
     * uid 0 (chroot path) we ignore DAC and write inside anyway; as
     * the app uid (proot path) the next file inside that dir EPERMs.
     * Toybox has no `--no-same-permissions`, so the cleanest fix is
     * doing the extract ourselves — defer dir-mode application to
     * after the children are written. Bonus: no FIFO, no shell.
     *
     * `tempFifo` is unused on this path. `format` is implicit in the
     * file extension that [ProotArchiveExtractor.openTarStream]
     * dispatches on.
     */
    override fun extractBootstrap(
        tarball: File,
        rootfs: String,
        format: BootstrapFormat,
        stripPrefix: String?,
        tempFifo: File,
        onLine: (String) -> Unit,
    ) {
        File(rootfs).mkdirs()
        ProotArchiveExtractor.extract(tarball, rootfs, stripPrefix, onLine)
    }

    /**
     * Recursive delete with one wrinkle: tracees from any historical
     * proot-via-su entry path can leave on-disk files with `uid=0`
     * ownership, even though the install was originally proot/app-uid.
     * Plain `chmod -R` from app uid then can't make those files
     * writable, and the subsequent unlink fails with EACCES.
     *
     * So: chmod via `su` if it's available, then delete. On
     * properly-rootless installs (no su ever touched the tree) the
     * `Su.run` call fails fast and we fall through to the same
     * app-uid chmod + recursive delete that always worked.
     */
    override fun wipe(installDir: File, log: (String) -> Unit) {
        if (!installDir.exists()) return
        val installPathQ = shellQuote(installDir.absolutePath)
        val rootfsPath = File(installDir, "rootfs").absolutePath
        val rootfsPathQ = shellQuote(rootfsPath)

        // Best-effort: kill any proot processes whose argv mentions
        // this rootfs. pkill -f scans /proc/<pid>/cmdline; we can only
        // see our own uid's processes (Android's hidepid policy), but
        // any leak from this app shows up there.
        log("kill: any in-flight proot for $rootfsPath (best-effort)")
        runShell(
            listOf("/system/bin/sh"),
            "pkill -KILL -f ${shellQuote("$prootBin .* $rootfsPath")} 2>/dev/null; exit 0",
            onLine = { log("kill: $it") },
        )

        // The Arch bootstrap leaves some dirs (e.g. ca-certificates'
        // `cadir`) at mode 0500 and files inside at 0444 — readable
        // but not writable, which means recursive delete can't
        // unlink the children. Chmod everything writable first, then
        // delete. -f silences "no such file" if a previous attempt
        // got partway. Prefer `su` when available so root-owned
        // tracee leftovers (see the kdoc above) get caught.
        log("chmod: making $installDir writable")
        val chmodScript = "chmod -R u+rwX $installPathQ 2>/dev/null; exit 0"
        if (Su.rootAvailable()) {
            val r = Su.run(chmodScript) { log("chmod (su): $it") }
            if (!r.ok) log("chmod (su): warning, exit=${r.exitCode}")
        } else {
            val r = runShell(listOf("/system/bin/sh"), chmodScript) { log("chmod: $it") }
            if (!r.ok) log("chmod: warning, exit=${r.exitCode}")
        }

        // Two-pass delete (rootfs subtree, then metadata + container)
        // for the same reason as [RootfsCleaner.wipe]: a cancel
        // mid-wipe must leave `metadata.json` intact so the home
        // screen still shows the slot and a follow-up uninstall picks
        // up cleanly. `find -xdev -depth -delete` rather than `rm -rf`
        // or `File.deleteRecursively()`:
        //   - -xdev refuses to cross filesystem boundaries, so even if
        //     something ever leaked a bind mount into this tree
        //     (proot itself doesn't, but a future regression / manual
        //     `su -c <chroot-enter>` against this rootfs could) we
        //     wouldn't start deleting `/apex` or `/system`.
        //   - -depth -delete walks post-order so directories are
        //     emptied before unlink, which neither `rm -rf` legacy
        //     toyboxes nor `File.deleteRecursively()` (which aborts
        //     on the first un-deletable child) can match.
        // Quoting flows through shellQuote to keep a hostile `id`
        // (already rejected by Installation.isValidId, but defence in
        // depth) from breaking out of the wrapper.
        log("delete: rootfs subtree at $rootfsPath")
        if (File(rootfsPath).exists()) {
            // No per-line `onLine` — `find -delete` over a multi-GB
            // rootfs floods the panel with one line per
            // permission-denied / busy file (especially on cancel),
            // and only the failure message is interesting. Full
            // output is still in `rfRes.output` for the IOException.
            val rfRes = runShell(
                listOf("/system/bin/sh"),
                "find $rootfsPathQ -xdev -depth -delete 2>&1",
                onLine = null,
            )
            if (!rfRes.ok || File(rootfsPath).exists()) {
                if (Su.rootAvailable()) {
                    log("delete: app-uid rootfs find failed, retrying via su")
                    val sr = Su.run("find $rootfsPathQ -xdev -depth -delete")
                    if (!sr.ok || File(rootfsPath).exists()) {
                        throw IOException(
                            "rootfs delete failed (su retry exit=${sr.exitCode}): ${sr.output}"
                        )
                    }
                } else {
                    throw IOException(
                        "rootfs delete failed (exit=${rfRes.exitCode}): ${rfRes.output}"
                    )
                }
            }
        }

        // Pass 2: explicit metadata.json + container rmdir. The
        // metadata.json unlink is the second-to-last visible artefact,
        // with only the empty installDir left and `rmdir` finishing
        // things near-atomically. See RootfsCleaner.wipe for the same
        // reasoning.
        log("delete: container at $installDir (metadata.json, rmdir)")
        val pass2Script = buildString {
            appendLine("rm -f $installPathQ/metadata.json.tmp")
            appendLine("rm -f $installPathQ/metadata.json")
            appendLine("rmdir $installPathQ")
        }
        val r = runShell(listOf("/system/bin/sh"), pass2Script, onLine = null)
        if (r.ok && !installDir.exists()) return

        // App-uid delete failed — almost certainly a residual
        // root-owned file from a historical su-mediated entry.
        // Retry once via `su`. The same explicit order applies.
        if (Su.rootAvailable()) {
            log("delete: app-uid pass-2 failed, retrying via su")
            val sr = Su.run(pass2Script)
            if (sr.ok && !installDir.exists()) return
            throw IOException("Recursive delete failed (su retry exit=${sr.exitCode}): ${sr.output}")
        }
        throw IOException("Recursive delete failed for $installDir (exit=${r.exitCode})")
    }

    // ---- internals ---------------------------------------------------

    /**
     * The standard proot argv that every in-rootfs invocation prefixes.
     * Doesn't include the user command — that's appended by
     * [startInside] separately.
     */
    private fun prootArgv(rootfs: String): List<String> = buildList {
        add(prootBin)
        addAll(listOf("-r", rootfs))
        addAll(listOf("-0", "-w", "/"))
        add("--kill-on-exit")
        add("--link2symlink")
        addAll(listOf("-b", "/dev"))
        addAll(listOf("-b", "/proc"))
        addAll(listOf("-b", "/sys"))
        // Override /dev/shm with an app-writable cache dir. Must come
        // after `-b /dev`; proot honours later binds for overlapping
        // paths. See [devShmDir] for why this is needed.
        addAll(listOf("-b", "$devShmDir:/dev/shm"))
        // libhybris bridges glibc-side mesa/Vulkan to bionic GPU
        // drivers, which live in /apex + /vendor + /system + the
        // bionic linker config under /linkerconfig. ChrootMounter
        // bind-mounts these for the chroot path; we mirror the same
        // set via proot's `-b` flag here. Bind targets that don't
        // exist on the host (e.g. /system_ext on older Androids,
        // /linkerconfig on pre-Q) are filtered out before this point
        // — see [LIBHYBRIS_BIND_DIRS].
        for (dir in LIBHYBRIS_BIND_DIRS) {
            addAll(listOf("-b", dir))
        }
        // Expose JUST the compositor's `share/` subdir at
        // /usr/share/tawc inside the rootfs — wayland socket and
        // Xwayland's xtmp dir live there. Deliberately not the whole
        // <appData> tree (which would expose libhybris's asset
        // extract, the proot scratch dir, and everything else under
        // <filesDir> to in-rootfs writes — see notes/installation.md
        // "/usr/share/tawc"). RootfsEnv sets WAYLAND_DISPLAY to the
        // in-rootfs path; no /tmp/wayland-0 symlink needed.
        addAll(listOf("-b", "$TAWC_SHARE:${TawcrootMethod.GUEST_TAWC_SHARE_DIR}"))
        // Surface Xwayland's listening socket at the canonical X11
        // path. Asymmetric bind, no in-rootfs symlink. Pre-created in
        // [startInside] so proot accepts the source. libxcb hardcodes
        // /tmp/.X11-unix/X<N> for the `:N` form of $DISPLAY, so we
        // can't just expose it via /usr/share/tawc.
        addAll(listOf("-b", "$TAWC_SHARE/xtmp/.X11-unix:/tmp/.X11-unix"))
    }

    /**
     * Run [script] under [argv] (typically `/system/bin/sh`). Pipes
     * the script in via stdin so we never quote-escape through the
     * shell. Mirrors [Su.run]'s shape to keep the two side-by-side
     * stories consistent.
     *
     * No `set -e` prefix here. The chroot path's [Su.run] does set -e
     * because the snippet it runs is a multi-step privileged setup
     * where any failure should abort. Our shell snippets are single
     * commands (a proot invocation) where the exit code IS the
     * result, and `set -e` would race the merged-stderr drain — proot
     * writing diagnostics to fd 2 sometimes loses them when the
     * shell exits before the kernel has flushed our reader.
     */
    private fun runShell(
        argv: List<String>,
        script: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        val pb = ProcessBuilder(argv).redirectErrorStream(true)
        // Set PROOT_TMP_DIR for any `runShell` invocation that happens
        // to wrap proot — it's harmless when the script doesn't.
        pb.environment()["PROOT_TMP_DIR"] = prootTmpDir
        pb.environment()["PROOT_LOADER"] = prootLoader
        val proc = pb.start()
        proc.outputStream.bufferedWriter().use { w ->
            w.write(script)
            w.write("\n")
        }
        return collectOutput(proc, onLine)
    }

    private fun collectOutput(
        proc: Process,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        val sb = StringBuilder()
        val reader = proc.inputStream.bufferedReader()
        val readerThread = Thread {
            try {
                reader.forEachLine { line ->
                    if (sb.length < 256 * 1024) {
                        if (sb.isNotEmpty()) sb.append('\n')
                        sb.append(line)
                    }
                    onLine?.invoke(line)
                }
            } catch (e: IOException) {
                Log.w(TAG, "proot stdout reader: $e")
            }
        }.also { it.isDaemon = true; it.start() }
        try {
            proc.waitFor()
        } catch (e: InterruptedException) {
            proc.destroyForcibly()
            readerThread.join(2000)
            Thread.currentThread().interrupt()
            throw e
        }
        readerThread.join(2000)
        return MethodResult(proc.exitValue(), sb.toString())
    }

    companion object {
        const val KEY = "proot"
        private const val TAG = "tawc-install"
        /** Compositor's tawc-shared dir (wayland socket, Xwayland xtmp).
         *  Same shape as [TawcrootMethod.GUEST_TAWC_SHARE_DIR] —
         *  source kept identical between the two methods. */
        private const val TAWC_SHARE = "/data/data/me.phie.tawc/share"

        /**
         * Android paths bound into the proot view so libhybris can
         * dlopen bionic-side GPU libraries. Mirrors the bind set in
         * [me.phie.tawc.install.ChrootMounter.mountScript] minus the
         * ones already covered by `-b /dev` (binderfs) and the chroot-
         * only `mount --rbind` recursion of /apex sub-mounts (proot
         * passes through sub-mounts naturally because bind is path
         * rewriting, not a real mount).
         *
         * Filtered at class load to only paths that currently exist
         * on the host — proot rejects the whole argv if any bind
         * source is missing, and `/system_ext` / `/linkerconfig`
         * weren't introduced until Android 11. The emulator has all
         * of these too, so the filter just guards against very old
         * device images, not against running on the emulator (where
         * libhybris itself is ABI-gated out — the libhybris asset
         * isn't shipped for x86_64).
         */
        val LIBHYBRIS_BIND_DIRS: List<String> = listOf(
            "/apex",
            "/vendor",
            "/system",
            "/system_ext",
            "/linkerconfig",
        ).filter { File(it).exists() }
    }
}
