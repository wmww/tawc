package me.phie.tawc.install

import android.content.Context
import me.phie.tawc.AppPaths
import me.phie.tawc.GraphicsBackend
import me.phie.tawc.Settings
import java.io.File
import java.io.IOException

/**
 * Rootless implementation of [InstallationMethod] using the
 * tawcroot binary (built by `tawcroot/build.sh`, shipped in the
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
 *     conflicts with proot's ptrace tracer; tawcroot instead denies
 *     guest seccomp filter installs with `EPERM`, which current
 *     Firefox accepts without UI warnings.
 *
 * See `notes/tawcroot/README.md` for the full design + phasing.
 */
class TawcrootMethod(context: Context) : InstallationMethod {
    private val appPaths = AppPaths.from(context)
    private val tawcShare: String = appPaths.shareDir.absolutePath
    private val store = InstallationStore(context)

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
        Sh.run("set -eu\n$script", onLine)

    /**
     * Start a tawcroot subprocess running [command] inside [rootfs].
     * Argv shape:
     *
     *   /system/bin/setsid <tawcroot> -r <rootfs> \
     *       -b /dev:/dev -b /proc:/proc -b /sys:/sys \
     *       -b /apex:/apex [-b /vendor:/vendor ...] \
     *       -b <appData>/share:/usr/share/tawc \
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
     * The in-rootfs bash starts under `/usr/bin/env -i KEY=VAL …` so
     * nothing the host JVM (or Android's launcher chain) inherited
     * leaks through — the bash sees exactly [RootfsEnv]'s map. `bash
     * -lc` still runs the distro-shipped /etc/profile + profile.d so
     * locale and package PATH additions still apply.
     */
    override fun startInside(rootfs: String, command: String?, graphics: GraphicsBackend?): Process {
        val externalBinds = externalBindsFor(rootfs)
        val andoHostDir = store.andoHostDir(rootfs)
        val tmpdir = prepareSpawn(rootfs, externalBinds)
        val argv = buildList {
            add("/system/bin/setsid")
            addAll(rootfsArgv(rootfs, graphics, externalBinds, andoHostDir))
            add("/bin/bash")
            if (command != null) {
                add("-lc"); add(command)
            } else {
                add("-l")
            }
        }
        return ProcessBuilder(argv)
            .directory(File(tmpdir))
            .also {
                it.environment().clear()
                it.environment()["TMPDIR"] = tmpdir
            }
            .start()
    }

    /**
     * Spawn parameters for an interactive in-rootfs login shell on a
     * caller-owned pty — the in-app terminal
     * ([me.phie.tawc.terminal.TerminalActivity]), whose termux
     * terminal-emulator JNI forks the pty pair and execs [argv]
     * directly. Same envelope as [startInside] minus the `setsid`
     * prefix: the pty spawn setsid()s the child itself, which both
     * upholds the rootfs-session invariant (notes/rootfs-sessions.md)
     * and makes the shell the session leader of the new pty so job
     * control works. TERM/COLORTERM ride after [RootfsEnv]'s map
     * because the pipe-stdio paths share that map and aren't ttys.
     *
     * [hostEnv] / [cwd] carry the same TMPDIR/tmpdir [startInside]
     * sets on the host-side tawcroot process (see [prepareSpawn]) —
     * though unlike [startInside]'s cleared ProcessBuilder env, the
     * termux JNI putenv()s entries over the inherited app environment.
     * Harmless: tawcroot reads no env vars and the guest env is fully
     * reset by `env -i`.
     */
    data class PtyExec(val argv: List<String>, val hostEnv: List<String>, val cwd: String)

    /**
     * [command] == null runs an interactive login shell (`-l`); else the
     * shell runs `-lc <command>` — still a login shell so profile env
     * fires, matching [startInside].
     */
    fun ptyShellExec(
        rootfs: String,
        graphics: GraphicsBackend? = null,
        command: String? = null,
    ): PtyExec {
        val externalBinds = externalBindsFor(rootfs)
        val andoHostDir = store.andoHostDir(rootfs)
        val tmpdir = prepareSpawn(rootfs, externalBinds)
        val argv = buildList {
            addAll(rootfsArgv(rootfs, graphics, externalBinds, andoHostDir))
            add("TERM=xterm-256color")
            add("COLORTERM=truecolor")
            add("/bin/bash")
            if (command != null) {
                add("-lc"); add(command)
            } else {
                add("-l")
            }
        }
        return PtyExec(argv, listOf("TMPDIR=$tmpdir"), tmpdir)
    }

    /**
     * Pre-create everything a tawcroot spawn expects on disk; returns
     * the host-side tmpdir (`<rootfs>/tmp`) the tawcroot process should
     * run in.
     *
     * Bind targets: tawcroot's `tawcroot_path_add_bind` opens the SRC
     * dir at startup; the in-rootfs DST dir doesn't need to exist (it's
     * purely a name-rewriting key). We still mkdir each to match the
     * proot path's habit, since some workflows assume the dst path
     * materializes on disk.
     *
     * Share dir: source for the X11-socket fake bind plus the wayland
     * socket dir. Compositor mkdirs the X11-unix subdir before
     * launching Xwayland too; recreating here is harmless and lets
     * pre-compositor entries (install steps, tests) bind it without
     * relying on launch order. The bare /share dir is also mkdir'd so
     * tawcroot can open it as the bind src on a fresh device before
     * the compositor has run.
     *
     * Tmpdir: TMPDIR points inside the rootfs so getcwd reverse-
     * translation works cleanly and daemons pacman-key spawns
     * (gpg-agent) get a sane writable tmp. Set on the host-side
     * tawcroot process — not propagated into the rootfs (env -i drops
     * it; the bash shell gets TMPDIR=/tmp from RootfsEnv instead).
     */
    private fun prepareSpawn(rootfs: String, externalBinds: List<ExternalBind>): String {
        File(rootfs, GUEST_TAWC_SHARE_DIR.removePrefix("/")).mkdirs()
        for (dir in LIBHYBRIS_BIND_DIRS) {
            File(rootfs, dir.removePrefix("/")).mkdirs()
        }
        for (bind in externalBinds) {
            File(rootfs, bind.guestPath.removePrefix("/")).mkdirs()
        }
        File(tawcShare).mkdirs()
        File("$tawcShare/xtmp/.X11-unix").mkdirs()
        val tmpdir = "$rootfs/tmp"
        File(tmpdir).mkdirs()
        return tmpdir
    }

    /** `<tawcroot> -r <rootfs> -b … -- /usr/bin/env -i -C /root K=V …`
     * — the shared spawn prefix up to (and including) the rootfs env;
     * callers append the program to run. */
    private fun rootfsArgv(
        rootfs: String,
        graphics: GraphicsBackend?,
        externalBinds: List<ExternalBind>,
        andoHostDir: String?,
    ): List<String> = buildList {
        add(tawcrootBin)
        addAll(listOf("-r", rootfs))
        for ((src, dst) in bindSpecs(externalBinds, andoHostDir)) addAll(listOf("-b", "$src:$dst"))
        add("--")
        addAll(RootfsEnv.envArgv(RootfsEnv.Method.TAWCROOT, graphics ?: Settings.graphicsBackend))
    }

    /**
     * The install's configured external binds, validated for spawn.
     * Resolves the install id from [rootfs]'s on-disk location
     * (`<distros>/<id>/rootfs`) so every spawn surface — broker
     * RUNINSIDE, RunCommandOp, the in-app terminal, install pipeline
     * steps — picks the binds up without threading [Installation]
     * through their call chains. A rootfs outside the store (shouldn't
     * happen in practice) has no metadata and gets no external binds.
     *
     * Fail closed (notes/external-binds.md): a structurally bad bind, a
     * missing host dir, or a revoked all-files grant refuses the spawn
     * with an actionable error instead of silently launching without
     * the bind — a session writing "into shared storage" that actually
     * lands in an app-private stand-in dir would be data loss at
     * uninstall time.
     */
    private fun externalBindsFor(rootfs: String): List<ExternalBind> {
        val id = store.idForRootfs(rootfs) ?: return emptyList()
        val binds = store.load(id)?.externalBinds ?: emptyList()
        for (bind in binds) {
            bind.validationError()?.let {
                throw IOException("external bind ${bind.guestPath}: $it (edit it under Manage binds)")
            }
            // Grant check first: without it, even stat on shared
            // storage can fail and the missing-dir error would
            // misdiagnose a revoked grant as a deleted directory.
            if (AllFilesAccess.requiresGrant(bind.hostPath) && !AllFilesAccess.granted()) {
                throw IOException(
                    "external bind ${bind.guestPath}: ${bind.hostPath} needs all-files access; " +
                        "grant it in system settings (Manage binds > Grant access) or remove the bind"
                )
            }
            if (!File(bind.hostPath).isDirectory) {
                // requiresGrant only knows the canonical shared-storage
                // prefixes; aliases like /mnt/user/0 stat as missing
                // when the grant is absent, so hint at that here.
                val grantHint = if (!AllFilesAccess.granted()) {
                    " (if this is shared storage, granting all-files access may be required)"
                } else ""
                throw IOException(
                    "external bind ${bind.guestPath}: host dir ${bind.hostPath} does not exist; " +
                        "create it or remove the bind under Manage binds$grantHint"
                )
            }
        }
        return binds
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

    // ---- internals ---------------------------------------------------

    /** The full bind list, in declared order, as `(src, dst)` pairs.
     *
     * Order: /dev → /proc → /sys → libhybris dirs → tawc share → X11.
     * No `/dev/shm` bind: tawcroot's SIGSYS handler emulates POSIX
     * shm in-process via memfd_create (`tawcroot/src/shm.c`).
     *
     * The tawc share bind exposes JUST `<appData>/share/` (wayland
     * socket, Xwayland's xtmp dir) at the in-rootfs canonical path
     * `/usr/share/tawc/`. Deliberately not the whole `<appData>` —
     * that would expose the libhybris asset extract, the proot
     * scratch dir, and everything else under `<filesDir>` to
     * in-rootfs writes. See notes/installation.md "/usr/share/tawc".
     *
     * The X11 bind also surfaces Xwayland's listening socket
     * (`<appData>/share/xtmp/.X11-unix/X<n>`) at the canonical
     * `/tmp/.X11-unix` path because libxcb hardcodes that path for
     * the `:N` form of `$DISPLAY`. Asymmetric (src ≠ dst) — tawcroot's
     * path-rewriting bind handles that natively.
     *
     * User-configured external binds (shared storage etc., see
     * [ExternalBind]) ride after every built-in bind so they can't
     * shadow the system/share set. */
    private fun bindSpecs(
        externalBinds: List<ExternalBind>,
        andoHostDir: String?,
    ): List<Pair<String, String>> = buildList {
        add("/dev" to "/dev")
        add("/proc" to "/proc")
        add("/sys" to "/sys")
        for (dir in LIBHYBRIS_BIND_DIRS) add(dir to dir)
        add(tawcShare to GUEST_TAWC_SHARE_DIR)
        // Per-distro ando socket dir ([InstallationStore.andoHostDir],
        // non-null only when ando is enabled, read fresh per spawn) at
        // its own guest path ([GUEST_ANDO_DIR]), deliberately NOT under
        // the shared /usr/share/tawc bind — a disabled guest must have
        // no path that falls through into the guest-writable shared
        // dir, or that reaches any ando socket.
        andoHostDir?.let { add(it to GUEST_ANDO_DIR) }
        add("$tawcShare/xtmp/.X11-unix" to "/tmp/.X11-unix")
        for (bind in externalBinds) add(bind.hostPath to bind.guestPath)
    }

    companion object {
        const val KEY = "tawcroot"
        /** In-rootfs path the share dir is exposed at. Single source
         *  of truth — also referenced by [RootfsEnv] (WAYLAND_DISPLAY)
         *  and the chroot/proot install methods. */
        const val GUEST_TAWC_SHARE_DIR = "/usr/share/tawc"

        /** In-rootfs dir the per-distro ando socket is exposed at (only
         *  when ando is enabled). Deliberately NOT under
         *  [GUEST_TAWC_SHARE_DIR]: the shared dir is bound into every
         *  rootfs and is guest-writable, so a socket path falling
         *  through it could be hijacked by a co-tenant distro. Its own
         *  top-level bind has no such fall-through. The client's
         *  `SOCKET_DEFAULT` (tawcroot/ando/src/ando.c) is
         *  `$GUEST_ANDO_DIR/ando.sock`. Single source of truth — also
         *  used by the proot/chroot bind builders. See notes/ando.md. */
        const val GUEST_ANDO_DIR = "/run/tawc-ando"

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
