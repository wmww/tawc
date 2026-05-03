package me.phie.tawc.install

import android.content.Context
import android.util.Log
import java.io.File
import java.io.IOException

/**
 * Rootless implementation of [InstallationMethod] using the
 * tawcroot binary (built by `tawcroot/build`, shipped in the
 * APK as `jniLibs/<abi>/libtawcroot.so` despite being a static
 * non-PIE ET_EXEC executable — Android's jniLib extractor matches on
 * filename only).
 *
 * tawcroot is the systrap-based fast successor to [ProotMethod]: same
 * fake-chroot envelope at meaningfully lower syscall overhead because
 * there's no ptrace tracer process. Path translation, fake-root
 * uid/stat, and Android-specific syscall fixups all happen in the
 * SIGSYS handler of the tracee thread, with no marshalling between
 * tracer and tracee.
 *
 * Compared to [ProotMethod]:
 *
 *   - No separate loader stub. `libtawcroot.so` is statically linked
 *     and contains both the runtime + the ELF loader.
 *   - Same `/dev/shm` bind to an app-writable cache dir as proot. The
 *     earlier "tawcroot emulates POSIX shm via memfd_create" plan
 *     never landed; absent the bind, Firefox's parent process uses
 *     POSIX `shm_open(3)` against `/dev/shm/...`, gets ENOENT, and
 *     `MOZ_RELEASE_ASSERT(mHandle.IsValid() && mMapping.IsValid())`
 *     hard-crashes early in startup.
 *   - No `--link2symlink` flag. Hardlink-EACCES → symlink fallback is
 *     built into the linkat handler (`syscalls_fs.c::link_with_symlink_fallback`).
 *   - No `MOZ_DISABLE_*_SANDBOX` env vars. Firefox's sandbox setup
 *     conflicts with proot's ptrace tracer; tawcroot doesn't have a
 *     tracer, so the sandbox can come up cleanly.
 *
 * See `notes/tawcroot.md` for the full design + phasing.
 */
class TawcrootMethod(context: Context) : InstallationMethod {
    /** Absolute path to the tawcroot binary on disk. */
    val tawcrootBin: String =
        File(context.applicationInfo.nativeLibraryDir, "libtawcroot.so").absolutePath

    /**
     * Host-side directory bound at `/dev/shm` inside the rootfs. Same
     * rationale as [ProotMethod.devShmDir]: Android's /dev has no
     * /dev/shm and Firefox's parent uses POSIX `shm_open(3)`, hard-
     * crashing on ENOENT. `filesDir` (not `cacheDir`) so Android won't
     * purge it under storage pressure mid-session.
     */
    private val devShmDir: String = File(context.filesDir, "tawcroot-dev-shm").absolutePath

    override val key: String = KEY
    override val displayName: String = "tawcroot (systrap, rootless)"
    override val requiresRoot: Boolean = false

    override fun isAvailable(context: Context): Boolean {
        val f = File(tawcrootBin)
        return f.exists() && f.canExecute()
    }

    /**
     * Run a host-side script as the app uid. Same shape as
     * [ProotMethod.runOutside] — the tawcroot rootfs is app-uid-owned
     * just like proot's, and host-side cleanup never needs to enter
     * the rootfs view.
     */
    override fun runOutside(
        script: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult =
        runShell(listOf("/system/bin/sh"), "set -eu\n$script", onLine)

    /**
     * Run [command] inside a tawcroot rootfs at [rootfs]. Argv shape:
     *
     *   tawcroot -r <rootfs> -b /apex -b /vendor -b /system \
     *            [-b /system_ext] [-b /linkerconfig] \
     *            -b /dev -b /proc -b /sys \
     *            -b /data/data/me.phie.tawc:/data/data/me.phie.tawc \
     *            -- /bin/bash -lc <command>
     *
     * Bind set mirrors [ProotMethod.prootArgv] minus the proot-only
     * tweaks (`/dev/shm`, link2symlink, kill-on-exit). The libhybris
     * bind dirs are filtered to existing host paths at class-load.
     *
     * `bash -lc` so the chroot's `/etc/profile.d/01-tawc.sh` runs
     * (PATH, LD_LIBRARY_PATH, WAYLAND_DISPLAY).
     */
    override fun runInside(
        rootfs: String,
        command: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        // Refresh enter.sh on every entry — same lifecycle contract
        // as ChrootMethod / ProotMethod (host launcher path needs an
        // up-to-date script).
        val enterFile = File(File(rootfs).parentFile, "enter.sh")
        enterFile.writeText(renderEnterScript(rootfs))
        enterFile.setExecutable(true, false)

        // Pre-create bind targets. tawcroot's `tawcroot_path_add_bind`
        // opens the SRC dir at startup; the in-rootfs DST dir doesn't
        // need to exist (it's purely a name-rewriting key). We still
        // mkdir each to match the proot path's habit, since some
        // workflows assume the dst path materializes on disk.
        File(rootfs, TAWC_DATA.removePrefix("/")).mkdirs()
        for (dir in LIBHYBRIS_BIND_DIRS) {
            File(rootfs, dir.removePrefix("/")).mkdirs()
        }
        // Backing dir for the /dev/shm bind. Must exist before
        // tawcroot_path_add_bind opens it.
        File(devShmDir).mkdirs()

        val cmdB64 = android.util.Base64.encodeToString(
            command.toByteArray(Charsets.UTF_8),
            android.util.Base64.NO_WRAP,
        )
        // Invoke through enter.sh — same shape as host-side launchers
        // (`client/tawc-chroot-run`). enter.sh's cd-into-$ROOTFS/tmp +
        // explicit env setup matters in two places: gpg-agent (and other
        // daemons pacman-key spawns) inherit a sane TMPDIR and HOME from
        // the in-chroot login shell, and the host kernel cwd is set
        // INSIDE the rootfs (not its parent) so getcwd reverse-translates
        // cleanly. Doing the tawcroot invocation here directly skipped
        // both, leaving gpg-agent in a busy-loop accept() while still
        // wired to the install service's session.
        //
        // setsid wraps the whole thing so daemons that pacman-key spawns
        // (gpg-agent specifically) get their own session, decoupled from
        // the install service's. Without this, gpg-agent inherits a
        // signal mask / pgrp combination that causes its main loop to
        // spin at 100% CPU instead of cleanly daemonising.
        val enterAbs = enterFile.absolutePath
        val tawcrootCmd =
            "exec setsid ${shellQuote(enterAbs)} ${shellQuote(cmdB64)} 2>&1 < /dev/null"
        return runShell(listOf("/system/bin/sh"), tawcrootCmd, onLine)
    }

    private fun shellQuote(s: String): String =
        "'" + s.replace("'", "'\\''") + "'"

    /**
     * Pure-Kotlin extract via [ProotArchiveExtractor]. Same rationale
     * as [ProotMethod.extractBootstrap] — toybox tar's eager
     * directory-mode application breaks app-uid extraction inside
     * mode-0500 dirs. The Kotlin extractor defers mode application
     * until after children are written.
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
     * Recursive delete. tawcroot doesn't track tracee processes the
     * way proot does — there's no tracer to leave PIDs around — so
     * the wipe is simpler than [ProotMethod.wipe]: no pkill, no
     * `su`-retry chmod path. Just chmod + find -delete.
     */
    override fun wipe(installDir: File, log: (String) -> Unit) {
        if (!installDir.exists()) return
        val installPathQ = shellQuote(installDir.absolutePath)

        log("chmod: making $installDir writable")
        runShell(
            listOf("/system/bin/sh"),
            "chmod -R u+rwX $installPathQ 2>/dev/null; exit 0",
        ) { log("chmod: $it") }

        log("delete: $installDir")
        val r = runShell(
            listOf("/system/bin/sh"),
            "find $installPathQ -xdev -depth -delete 2>&1",
        ) { log("rm: $it") }

        if (r.ok && !installDir.exists()) return
        // Same fallback as ProotMethod: `su -c` retry catches any
        // root-owned leftovers from interleaved chroot+tawcroot use.
        if (Su.rootAvailable()) {
            log("delete: app-uid find -delete failed, retrying via su")
            val sr = Su.run("find $installPathQ -xdev -depth -delete") {
                log("rm (su): $it")
            }
            if (sr.ok && !installDir.exists()) return
            throw IOException(
                "Recursive delete failed (su retry exit=${sr.exitCode}): ${sr.output}"
            )
        }
        throw IOException("Recursive delete failed for $installDir (exit=${r.exitCode})")
    }

    override fun enterScript(context: Context, rootfs: String): String =
        renderEnterScript(rootfs)

    // ---- internals ---------------------------------------------------

    /** The full bind list, in declared order, as `(src, dst)` pairs.
     *
     * Single source of truth for both [tawcrootArgv] (programmatic
     * invocation) and [renderEnterScript] (shell-script form) — adding
     * a bind here updates both call sites at once.
     *
     * Order: /dev → /proc → /sys → /dev/shm → libhybris dirs → TAWC_DATA.
     * `/dev/shm` must come AFTER `/dev` so tawcroot's path translator
     * picks the more-specific bind first (same semantics as proot's
     * later-binds-win rule for overlapping paths). See [devShmDir]
     * for why the /dev/shm bind exists at all. */
    private fun bindSpecs(): List<Pair<String, String>> = buildList {
        add("/dev" to "/dev")
        add("/proc" to "/proc")
        add("/sys" to "/sys")
        add(devShmDir to "/dev/shm")
        for (dir in LIBHYBRIS_BIND_DIRS) add(dir to dir)
        add(TAWC_DATA to TAWC_DATA)
    }

    /** Standard tawcroot argv that every in-rootfs invocation prefixes.
     *
     * Note: tawcroot's `-b` always takes `src:dst` form (unlike proot,
     * which accepts a bare `-b /dev` to mean `-b /dev:/dev`). Pass the
     * full `src:dst` for every entry. */
    private fun tawcrootArgv(rootfs: String): List<String> = buildList {
        add(tawcrootBin)
        addAll(listOf("-r", rootfs))
        for ((src, dst) in bindSpecs()) addAll(listOf("-b", "$src:$dst"))
        add("--")
    }

    /**
     * Body of the per-install `enter.sh` for tawcroot mode. Runs as
     * the app uid (no `su`); host-side launchers wrap with
     * `run-as me.phie.tawc <enter.sh> <b64>`.
     */
    private fun renderEnterScript(rootfs: String): String = buildString {
        appendLine("#!/system/bin/sh")
        appendLine("# Auto-generated by TawcrootMethod.renderEnterScript. Do not")
        appendLine("# edit by hand — rewritten on every install / chroot entry.")
        appendLine("set -eu")
        // Same TMPDIR dance as ProotMethod — `run-as` starts mksh in
        // a cwd where here-doc temp file creation can fail.
        appendLine("export TMPDIR=${shellQuote(rootfs)}/tmp")
        appendLine("mkdir -p \"\$TMPDIR\" 2>/dev/null || true")
        appendLine("cd \"\$TMPDIR\" 2>/dev/null || true")
        appendLine("TAWCROOT=${shellQuote(tawcrootBin)}")
        appendLine("ROOTFS=${shellQuote(rootfs)}")
        appendLine("TAWC_DATA=${shellQuote(TAWC_DATA)}")
        appendLine("DEV_SHM_DIR=${shellQuote(devShmDir)}")
        // Bind targets must exist on disk inside the rootfs view since
        // some downstream code stat()'s them. Match ProotMethod.
        appendLine("mkdir -p \"\$ROOTFS\$TAWC_DATA\"")
        for (dir in LIBHYBRIS_BIND_DIRS) {
            appendLine("mkdir -p \"\$ROOTFS$dir\"")
        }
        // Host-side backing dir for the /dev/shm bind.
        appendLine("mkdir -p \"\$DEV_SHM_DIR\"")
        // Refresh /etc/profile.d/01-tawc.sh. Note: tawcroot does NOT
        // need the MOZ_DISABLE_*_SANDBOX env vars proot does — the
        // ptrace-tracer-vs-Firefox-sandbox conflict doesn't exist
        // here. Firefox can keep its sandboxes on under tawcroot.
        appendLine(
            """
            mkdir -p "${'$'}ROOTFS/etc/profile.d"
            cat > "${'$'}ROOTFS/etc/profile.d/01-tawc.sh" <<'TAWC_PROF_EOF'
            export WAYLAND_DISPLAY=/data/data/me.phie.tawc/wayland-0
            export XDG_RUNTIME_DIR=/tmp
            export LD_LIBRARY_PATH=/usr/local/lib/gl-shims:/usr/local/lib
            export HYBRIS_EGLPLATFORM=wayland
            export DISPLAY=:0
            # SDL2 prefers X11 whenever DISPLAY is set, but our Xwayland
            # is GLAMOR-disabled — SDL apps that probe the X11 backend
            # would bind our X server and immediately fail. Force the
            # Wayland-first ordering so SDL takes the libhybris/EGL
            # Wayland path. X11 stays in the list as a fallback (matches
            # ChrootMounter).
            export SDL_VIDEODRIVER=wayland,x11
            ln -sf /data/data/me.phie.tawc/wayland-0 /tmp/wayland-0 2>/dev/null
            # X11 sockets land in the host's /data/data/me.phie.tawc/xtmp/
            # (Android has no /tmp). The chroot's bind on /data/data/me.phie.tawc
            # makes them visible at the same path inside; surface them at the
            # canonical /tmp/.X11-unix/ so X clients with bare `DISPLAY=:0`
            # find them without per-app `unix:/path` overrides.
            # Use ln -sfn so re-running this with the dir already linked
            # doesn't dereference into a nested .X11-unix/.X11-unix.
            ln -sfn /data/data/me.phie.tawc/xtmp/.X11-unix /tmp/.X11-unix 2>/dev/null
            for lock in /data/data/me.phie.tawc/xtmp/.X*-lock; do
                [ -f "${'$'}lock" ] || continue
                ln -sf "${'$'}lock" "/tmp/${'$'}{lock##*/}" 2>/dev/null
            done
            TAWC_PROF_EOF
            chmod 644 "${'$'}ROOTFS/etc/profile.d/01-tawc.sh"
            """.trimIndent(),
        )
        // The tawcroot invocation. Bind list rendered from
        // [bindSpecs] so the order matches [tawcrootArgv] exactly.
        // Each path goes through shellQuote so future additions with
        // shell metachars don't break out.
        val bindFlags = bindSpecs().joinToString(" \\\n                    ") {
            (src, dst) -> "-b ${shellQuote("$src:$dst")}"
        }
        appendLine(
            """
            if [ ${'$'}# -gt 0 ] && [ -n "${'$'}1" ]; then
                CMD=${'$'}(printf %s "${'$'}1" | base64 -d)
                exec "${'$'}TAWCROOT" -r "${'$'}ROOTFS" \
                    $bindFlags \
                    -- /bin/bash -lc "${'$'}CMD"
            else
                exec "${'$'}TAWCROOT" -r "${'$'}ROOTFS" \
                    $bindFlags \
                    -- /bin/bash -l
            fi
            """.trimIndent(),
        )
    }

    private fun runShell(
        argv: List<String>,
        script: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        val pb = ProcessBuilder(argv).redirectErrorStream(true)
        val proc = pb.start()
        proc.outputStream.bufferedWriter().use { w ->
            w.write(script); w.write("\n")
        }
        val sb = StringBuilder()
        val readerThread = Thread {
            try {
                proc.inputStream.bufferedReader().forEachLine { line ->
                    if (sb.length < 256 * 1024) {
                        if (sb.isNotEmpty()) sb.append('\n')
                        sb.append(line)
                    }
                    onLine?.invoke(line)
                }
            } catch (e: IOException) {
                Log.w(TAG, "tawcroot stdout reader: $e")
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
        const val KEY = "tawcroot"
        private const val TAG = "tawc-install"
        private const val TAWC_DATA = "/data/data/me.phie.tawc"

        /** Same set as ProotMethod (kept in sync deliberately —
         * libhybris dlopen targets bionic GPU libraries via these). */
        val LIBHYBRIS_BIND_DIRS: List<String> = listOf(
            "/apex",
            "/vendor",
            "/system",
            "/system_ext",
            "/linkerconfig",
        ).filter { File(it).exists() }
    }
}
