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
 *   - Real in-memory `/dev/shm`: `shm_open(3)` calls under `/dev/shm/`
 *     are intercepted by the SIGSYS handler and routed to a
 *     `memfd_create`-backed name table (`tawcroot/src/shm.c`). No host
 *     directory is bound; the segments live in the kernel and dissolve
 *     when the last fd closes, so there's no flash-write cost or
 *     storage-growth concern. (Mozilla's parent → content IPC uses
 *     SCM_RIGHTS to pass the fd; no cross-process open-by-name needed.)
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
     * Start a tawcroot subprocess running [command] inside [rootfs].
     * Argv shape:
     *
     *   /system/bin/setsid <tawcroot> -r <rootfs> \
     *       -b /dev:/dev -b /proc:/proc -b /sys:/sys \
     *       -b /apex:/apex [-b /vendor:/vendor ...] \
     *       -b /data/data/me.phie.tawc:/data/data/me.phie.tawc \
     *       -- /bin/bash -lc <command>
     *
     * Bind set mirrors [ProotMethod] minus the proot-only tweaks
     * (`/dev/shm`, link2symlink, kill-on-exit). Libhybris bind dirs
     * are filtered to existing host paths at class-load.
     *
     * `setsid` upholds the rootfs-session invariant
     * (notes/rootfs-sessions.md): every chroot invocation runs in
     * its own session. The visible symptoms are gpg-agent's main
     * loop spinning at 100% CPU under pacman-key (inherited pgrp +
     * signal mask) and the integration test framework's PGID-based
     * cleanup; the underlying contract is general.
     *
     * `bash -lc` so the chroot's `/etc/profile.d/01-tawc.sh` runs
     * (PATH, LD_LIBRARY_PATH, WAYLAND_DISPLAY). The profile is
     * refreshed below before the spawn.
     */
    override fun startInside(rootfs: String, command: String?): Process {
        // Pre-create bind targets. tawcroot's `tawcroot_path_add_bind`
        // opens the SRC dir at startup; the in-rootfs DST dir doesn't
        // need to exist (it's purely a name-rewriting key). We still
        // mkdir each to match the proot path's habit, since some
        // workflows assume the dst path materializes on disk.
        File(rootfs, TAWC_DATA.removePrefix("/")).mkdirs()
        for (dir in LIBHYBRIS_BIND_DIRS) {
            File(rootfs, dir.removePrefix("/")).mkdirs()
        }
        // Refresh /etc/profile.d/01-tawc.sh so changes to the Wayland
        // env take effect without reinstalling.
        File(rootfs, "etc/profile.d").mkdirs()
        File(rootfs, "etc/profile.d/01-tawc.sh")
            .writeText(RootfsProfile.build(RootfsProfile.Method.TAWCROOT))

        val argv = buildList {
            add("/system/bin/setsid")
            add(tawcrootBin)
            addAll(listOf("-r", rootfs))
            for ((src, dst) in bindSpecs()) addAll(listOf("-b", "$src:$dst"))
            add("--")
            add("/bin/bash")
            if (command != null) {
                add("-lc"); add(command)
            } else {
                add("-l")
            }
        }
        // TMPDIR points inside the rootfs so getcwd reverse-translation
        // works cleanly and daemons pacman-key spawns (gpg-agent) get
        // a sane writable tmp.
        val tmpdir = "$rootfs/tmp"
        File(tmpdir).mkdirs()
        return ProcessBuilder(argv)
            .directory(File(tmpdir))
            .also { it.environment()["TMPDIR"] = tmpdir }
            .start()
    }

    private fun shellQuote(s: String): String =
        "'" + s.replace("'", "'\\''") + "'"

    /** Used by [runOutside] / [wipe]: run a short shell script via
     * `argv` (typically `/system/bin/sh`), feed [script] on stdin,
     * collect combined stdout+stderr. */
    private fun runShell(
        argv: List<String>,
        script: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        val pb = ProcessBuilder(argv).redirectErrorStream(true)
        val proc = pb.start()
        proc.outputStream.bufferedWriter().use { w -> w.write(script); w.write("\n") }
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
                Log.w(TAG, "runShell stdout: $e")
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
            onLine = null,
        )

        log("delete: $installDir")
        // No per-line callback — `find -delete` over a multi-GB
        // rootfs streams one stderr line per permission-denied /
        // busy file (very common on cancel paths) and floods the
        // panel. Full output is captured in `r.output` for the
        // IOException below.
        val r = runShell(
            listOf("/system/bin/sh"),
            "find $installPathQ -xdev -depth -delete 2>&1",
            onLine = null,
        )

        if (r.ok && !installDir.exists()) return
        // Same fallback as ProotMethod: `su -c` retry catches any
        // root-owned leftovers from interleaved chroot+tawcroot use.
        if (Su.rootAvailable()) {
            log("delete: app-uid find -delete failed, retrying via su")
            val sr = Su.run("find $installPathQ -xdev -depth -delete")
            if (sr.ok && !installDir.exists()) return
            throw IOException(
                "Recursive delete failed (su retry exit=${sr.exitCode}): ${sr.output}"
            )
        }
        throw IOException("Recursive delete failed for $installDir (exit=${r.exitCode})")
    }

    // ---- internals ---------------------------------------------------

    /** The full bind list, in declared order, as `(src, dst)` pairs.
     *
     * Order: /dev → /proc → /sys → libhybris dirs → TAWC_DATA.
     * No `/dev/shm` bind: tawcroot's SIGSYS handler emulates POSIX
     * shm in-process via memfd_create (`tawcroot/src/shm.c`). */
    private fun bindSpecs(): List<Pair<String, String>> = buildList {
        add("/dev" to "/dev")
        add("/proc" to "/proc")
        add("/sys" to "/sys")
        for (dir in LIBHYBRIS_BIND_DIRS) add(dir to dir)
        add(TAWC_DATA to TAWC_DATA)
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
