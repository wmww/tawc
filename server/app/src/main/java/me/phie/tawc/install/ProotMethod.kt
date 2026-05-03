package me.phie.tawc.install

import android.content.Context
import android.util.Log
import java.io.File
import java.io.IOException

/**
 * Rootless implementation of [InstallationMethod] using the vendored
 * `proot` binary (built by `client/build-proot`, shipped in the APK
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
     *   - `-b <appData>:<appData>` — same path inside and outside so
     *     `WAYLAND_DISPLAY` (a unix socket at
     *     `/data/data/me.phie.tawc/wayland-0`) is reachable. The chroot
     *     profile.d symlinks `/tmp/wayland-0` → that path.
     *   - `--link2symlink`      — turn hardlink calls into symlink
     *     calls. Pacman occasionally hardlinks across mounts; proot
     *     can't always satisfy that on Android.
     *   - `--kill-on-exit`      — SIGKILL the descendant tree if the
     *     proot leader dies. Without this, a crashed `pacman` can
     *     leave `gpg-agent` running (same problem chroot had — see
     *     RootfsCleaner).
     *
     * Command goes via `bash -lc` so the profile.d entries fire
     * (`PATH`, `LD_LIBRARY_PATH`, Wayland env from `01-tawc.sh`).
     *
     * Refreshes `enter.sh` on every call so any changes to this
     * rendering take effect without reinstalling — same contract as
     * [ChrootMethod.runInside].
     */
    override fun runInside(
        rootfs: String,
        command: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        // Refresh enter.sh; cheap and keeps host-side `tawc-chroot-run`
        // in sync. The host launcher needs an enter.sh on disk anyway;
        // in-app callers go through this argv path directly.
        val enterFile = File(File(rootfs).parentFile, "enter.sh")
        enterFile.writeText(renderEnterScript(rootfs))
        enterFile.setExecutable(true, false)

        // Pre-create the bind targets and proot's scratch dir. proot
        // refuses to bind to a guest path that doesn't exist on disk,
        // so we materialise `<rootfs>/data/data/<pkg>` (the wayland
        // socket bind) and the libhybris bind mounts (see
        // [LIBHYBRIS_BIND_DIRS]) before invoking it.
        File(prootTmpDir).mkdirs()
        File(devShmDir).mkdirs()
        File(rootfs, TAWC_DATA.removePrefix("/")).mkdirs()
        for (dir in LIBHYBRIS_BIND_DIRS) {
            File(rootfs, dir.removePrefix("/")).mkdirs()
        }

        // We invoke proot via the system shell rather than direct
        // ProcessBuilder argv. Direct ProcessBuilder exec of the proot
        // binary from app context produces a silent exit-255 (process
        // forks, the loader stub fails to execve_no_trans through
        // SELinux+seccomp gauntlet, but with redirectErrorStream and
        // no shell wrapping we can't see why). Going through
        // `/system/bin/sh -c '<proot args>'` makes the shell handle
        // exec for us — Android's app seccomp policy allows that
        // path and we still get any errors back via stdout. The
        // user's command is base64-encoded so we don't have to worry
        // about quoting it through this extra shell layer.
        val cmdB64 = android.util.Base64.encodeToString(
            command.toByteArray(Charsets.UTF_8),
            android.util.Base64.NO_WRAP,
        )
        val debugLog = "$prootTmpDir/last-run.log"
        val prootCmd = buildString {
            // PROOT_TMP_DIR is /tmp by default and /tmp isn't writable
            // by the app uid on Android.
            append("export PROOT_TMP_DIR=").append(shellQuote(prootTmpDir)).append("; ")
            // PROOT_LOADER points at our vendored loader stub so proot
            // skips the extract-to-tmp+execve dance (blocked by
            // Android's no-exec-in-app-home-dir rule for API 29+).
            append("export PROOT_LOADER=").append(shellQuote(prootLoader)).append("; ")
            // Tee everything proot prints into a side log so we can
            // read it after the run; with pipe quirks we sometimes
            // lose proot's last lines via the merged-fd reader alone.
            append("(")
            for (arg in prootArgv(rootfs)) {
                append(shellQuote(arg)); append(' ')
            }
            // Bash inside proot decodes the base64 payload and evals
            // it. Single-quoting the whole bash script keeps the outer
            // /system/bin/sh from interpreting `$(...)` itself before
            // proot ever sees the command.
            append("/bin/bash -lc 'eval \"\$(printf %s ")
            append(cmdB64)
            append(" | base64 -d)\"'")
            append(") 2>&1 | tee ").append(shellQuote(debugLog))
            // Read PIPESTATUS[0] (proot's exit) — without it the
            // outer shell's $? would be tee's, which is always 0.
            // Both bash and mksh expose ${PIPESTATUS[N]} this way.
            append("; rc=\${PIPESTATUS[0]}; echo \"proot exit=\$rc\" >> ").append(shellQuote(debugLog))
            append("; exit \$rc")
        }
        val r = runShell(listOf("/system/bin/sh"), prootCmd, onLine)
        if (!r.ok) {
            // Best-effort: read the side log so callers see whatever
            // proot did manage to print before disappearing.
            val sideLog = try { File(debugLog).readText().take(4096) } catch (_: Exception) { "(no side log)" }
            Log.e(TAG, "proot failed; side log:\n$sideLog")
        }
        return r
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
     * Recursive delete with one wrinkle: tracees that ran under a
     * proot-via-su entry (e.g. integration tests that go through
     * `client/tawc-chroot-run`'s `su -c '<enter.sh>'` path on rooted
     * devices) leave on-disk files with `uid=0` ownership, even though
     * the install was originally proot/app-uid. Plain `chmod -R` from
     * app uid then can't make those files writable, and the
     * subsequent unlink fails with EACCES.
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

        // Pass 2: explicit `enter.sh` → `metadata.json.tmp` →
        // `metadata.json` → `rmdir` order so the `metadata.json`
        // unlink is the second-to-last visible artefact, with only
        // the empty installDir left and `rmdir` finishing things
        // near-atomically. See RootfsCleaner.wipe for the same
        // reasoning.
        log("delete: container at $installDir (enter.sh, metadata.json, rmdir)")
        val pass2Script = buildString {
            appendLine("rm -f $installPathQ/enter.sh")
            appendLine("rm -f $installPathQ/metadata.json.tmp")
            appendLine("rm -f $installPathQ/metadata.json")
            appendLine("rmdir $installPathQ")
        }
        val r = runShell(listOf("/system/bin/sh"), pass2Script, onLine = null)
        if (r.ok && !installDir.exists()) return

        // App-uid delete failed — almost certainly a residual
        // root-owned file from a `su -c '<enter.sh>'` invocation.
        // Retry once via `su`. The same explicit order applies.
        if (Su.rootAvailable()) {
            log("delete: app-uid pass-2 failed, retrying via su")
            val sr = Su.run(pass2Script)
            if (sr.ok && !installDir.exists()) return
            throw IOException("Recursive delete failed (su retry exit=${sr.exitCode}): ${sr.output}")
        }
        throw IOException("Recursive delete failed for $installDir (exit=${r.exitCode})")
    }

    override fun enterScript(context: Context, rootfs: String): String =
        renderEnterScript(rootfs)

    // ---- internals ---------------------------------------------------

    /**
     * The standard proot argv that every in-rootfs invocation prefixes.
     * Doesn't include the user command — that's appended by
     * [runInside] / `enter.sh` separately.
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
        // Compositor's data dir is at the same absolute path inside
        // the rootfs view so the wayland-0 socket path is valid in
        // both worlds. (See ChrootMounter.mountScript for the chroot
        // counterpart.) Both paths share `/data/data/me.phie.tawc`
        // and the `01-tawc.sh` profile inside the rootfs symlinks
        // `/tmp/wayland-0` to that location.
        addAll(listOf("-b", "$TAWC_DATA:$TAWC_DATA"))
    }

    /**
     * Body of the per-install `enter.sh` for proot mode. Runs as the
     * app uid (no `su`) so host-side launchers wrap with
     * `run-as me.phie.tawc <enter.sh> <b64>` instead of
     * `su -c '<enter.sh> <b64>'`. The b64 payload is decoded inside
     * the rootfs's bash so the same single-quoted layered-quoting trick
     * we use for chroot still works.
     */
    private fun renderEnterScript(rootfs: String): String = buildString {
        appendLine("#!/system/bin/sh")
        appendLine("# Auto-generated by ProotMethod.renderEnterScript. Do not edit by hand —")
        appendLine("# rewritten on every install / chroot entry.")
        appendLine("set -eu")
        // Host-side launchers (`adb shell run-as me.phie.tawc enter.sh`)
        // start mksh with cwd=/data/local, where it has no write
        // permission for here-doc temp files. Force TMPDIR + cwd to a
        // location app uid can write. We create proot-tmp first, since
        // it might have been purged by Android's cache cleanup.
        // Every path-bearing variable goes through shellQuote so a
        // future install id with a quote character (or any other shell
        // metacharacter) can't break out of the single-quoted literal.
        // Today's nativeLibraryDir + cacheDir + rootfs paths don't
        // contain quotes, but the abstraction shouldn't depend on that.
        appendLine("mkdir -p ${shellQuote(prootTmpDir)}")
        appendLine("export TMPDIR=${shellQuote(prootTmpDir)}")
        appendLine("cd \"\$TMPDIR\" 2>/dev/null || true")
        appendLine("PROOT=${shellQuote(prootBin)}")
        appendLine("ROOTFS=${shellQuote(rootfs)}")
        appendLine("TAWC_DATA=${shellQuote(TAWC_DATA)}")
        // proot writes a per-invocation loader to its scratch dir;
        // /tmp isn't writable by the app uid on Android, so we point
        // at the app's cacheDir and ensure it exists on every entry
        // (Android may purge it under storage pressure).
        appendLine("PROOT_TMP_DIR=${shellQuote(prootTmpDir)}")
        appendLine("PROOT_LOADER=${shellQuote(prootLoader)}")
        appendLine("export PROOT_TMP_DIR PROOT_LOADER")
        appendLine("mkdir -p \"\$PROOT_TMP_DIR\"")
        // Backing store for the `/dev/shm` bind below — see [devShmDir].
        appendLine("DEV_SHM_DIR=${shellQuote(devShmDir)}")
        appendLine("mkdir -p \"\$DEV_SHM_DIR\"")
        // Materialise every bind target before proot looks for it.
        // proot refuses bind dst paths that don't resolve on disk.
        // Wayland socket dir, plus the libhybris-related Android
        // sysfs/apex mounts (see [LIBHYBRIS_BIND_DIRS]).
        appendLine("mkdir -p \"\$ROOTFS\$TAWC_DATA\"")
        for (dir in LIBHYBRIS_BIND_DIRS) {
            appendLine("mkdir -p \"\$ROOTFS${dir}\"")
        }
        // Refresh /etc/profile.d/01-tawc.sh on every entry, mirroring
        // ChrootMounter.mountScript. This is the env that makes
        // WAYLAND_DISPLAY / LD_LIBRARY_PATH / XDG_RUNTIME_DIR sane for
        // the in-chroot bash; refreshing here lets us tweak it without
        // reinstalling.
        appendLine(
            """
            mkdir -p "${'$'}ROOTFS/etc/profile.d"
            cat > "${'$'}ROOTFS/etc/profile.d/01-tawc.sh" <<'TAWC_PROF_EOF'
            # WAYLAND_DISPLAY is the absolute path to the socket
            # (rather than the bare display name + symlinked
            # /tmp/wayland-0 trick the chroot path uses). On proot,
            # connect(2) on the symlink fails with EPERM — the
            # symlink's target is interpreted relative to the host
            # filesystem root and the resulting path bypasses proot's
            # `-b /data/data/<pkg>` rewrite. Passing the absolute
            # path directly hits the bind translation cleanly.
            export WAYLAND_DISPLAY=/data/data/me.phie.tawc/wayland-0
            export XDG_RUNTIME_DIR=/tmp
            export LD_LIBRARY_PATH=/usr/local/lib/gl-shims:/usr/local/lib
            export HYBRIS_EGLPLATFORM=wayland
            # Maintain a /tmp/wayland-0 symlink alongside the absolute
            # path. WAYLAND_DISPLAY ignores it (it's an absolute path
            # already), but `testing/run-integration-tests.sh` and
            # `notes/`-documented one-liners use `test -e
            # <rootfs>/tmp/wayland-0` as a compositor-ready probe,
            # and the symlink target follows back to the live socket.
            ln -sf /data/data/me.phie.tawc/wayland-0 /tmp/wayland-0 2>/dev/null
            # Disable Firefox's per-subprocess seccomp/namespace
            # sandboxes. Firefox's content / GPU / RDD / Utility / GMP /
            # socket / VR processes set up their own seccomp filters and
            # PID/user namespaces during startup; under proot every
            # process is already a ptrace tracee with proot's own
            # syscall filter active, and Firefox's sandbox setup
            # SIGSEGVs every subprocess immediately ("[Parent] WARNING:
            # process N exited on signal 11"). Disabling them lets the
            # subprocesses come up; the chroot path doesn't need this
            # because there's no ptrace tracer there. Less secure in
            # principle, but the whole rootfs is already an app-uid
            # sandbox.
            export MOZ_DISABLE_CONTENT_SANDBOX=1
            export MOZ_DISABLE_GPU_SANDBOX=1
            export MOZ_DISABLE_RDD_SANDBOX=1
            export MOZ_DISABLE_SOCKET_PROCESS_SANDBOX=1
            export MOZ_DISABLE_UTILITY_SANDBOX=1
            export MOZ_DISABLE_GMP_SANDBOX=1
            export MOZ_DISABLE_VR_SANDBOX=1
            TAWC_PROF_EOF
            chmod 644 "${'$'}ROOTFS/etc/profile.d/01-tawc.sh"
            """.trimIndent(),
        )
        // The proot invocation. The libhybris bind list is rendered
        // inline once, kept identical to [prootArgv] so the in-app
        // and host-launcher paths see the same chroot view. shellQuote
        // each path so a future addition with a metachar can't break
        // the script (today's hardcoded paths are safe; this is just
        // not relying on that).
        val libhybrisBindFlags = LIBHYBRIS_BIND_DIRS.joinToString(" ") { "-b ${shellQuote(it)}" }
        appendLine(
            """
            if [ ${'$'}# -gt 0 ] && [ -n "${'$'}1" ]; then
                CMD=${'$'}(printf %s "${'$'}1" | base64 -d)
                exec "${'$'}PROOT" -r "${'$'}ROOTFS" -0 -w / \
                    --kill-on-exit --link2symlink \
                    -b /dev -b /proc -b /sys \
                    -b "${'$'}DEV_SHM_DIR:/dev/shm" \
                    $libhybrisBindFlags \
                    -b "${'$'}TAWC_DATA:${'$'}TAWC_DATA" \
                    /bin/bash -lc "${'$'}CMD"
            else
                exec "${'$'}PROOT" -r "${'$'}ROOTFS" -0 -w / \
                    --kill-on-exit --link2symlink \
                    -b /dev -b /proc -b /sys \
                    -b "${'$'}DEV_SHM_DIR:/dev/shm" \
                    $libhybrisBindFlags \
                    -b "${'$'}TAWC_DATA:${'$'}TAWC_DATA" \
                    /bin/bash -l
            fi
            """.trimIndent(),
        )
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
        private const val TAWC_DATA = "/data/data/me.phie.tawc"

        /**
         * Android paths bound into the proot view so libhybris can
         * dlopen bionic-side GPU libraries (see
         * [LibhybrisLinker]). Mirrors the bind set in
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
         * libhybris itself is ABI-gated out by [LibhybrisLinker]).
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
