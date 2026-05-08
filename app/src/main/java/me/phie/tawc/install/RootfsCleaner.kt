package me.phie.tawc.install

import me.phie.tawc.tasks.ProcessScanner
import java.io.File
import java.io.IOException

/**
 * The one and only path that deletes anything under `<distros>/<id>/`.
 *
 * The install pipeline never wipes (the state-machine gate guarantees
 * `install` only runs against a `(no dir)` slot), so every byte that
 * leaves the rootfs goes through here. The sequence is:
 *
 *   1. **Kill chroot processes** via [ProcessScanner.killAllInRootfs]
 *      (`includeChroot=true`). The canonical leak is the
 *      `gpg-agent --daemon` that `pacman-key --init` detaches; left
 *      alive it holds FDs into the rootfs and races the delete, which
 *      on Android 14 spins vold's FUSE accounting into a `vdc volume
 *      abort_fuse` storm bad enough to ANR system_server. The scanner
 *      sends KILL twice with a beat between to catch fork-on-signal
 *      respawns.
 *   2. **Strict unmount** via [ChrootMounter.unmount] (mount-master
 *      mode). Refuses with a non-zero exit if any mount remains under
 *      the rootfs — we'd rather leave a `FAILED` install on disk than
 *      `find` through a live `/dev` bind.
 *   3. **`find -xdev -depth -delete`** — never `rm -rf`, since toybox
 *      `rm` has no `--one-file-system`. `-xdev` is belt-and-braces
 *      against any mount [ChrootMounter.unmount] missed (e.g. a leak
 *      via `scripts/tawc-rootfs-run.sh` that bound `/dev` at runtime).
 *      The delete is split into **two passes** so a cancel mid-wipe
 *      can never strand the slot without `metadata.json`: pass 1
 *      walks the rootfs subtree only, pass 2 tears down `metadata.json`
 *      / `enter.sh` / the parent dir once we know the heavy work is
 *      done. `find -depth` already orders deletion leaf-first within a
 *      single tree, but the install-dir-level ordering between
 *      `rootfs/` and its sibling `metadata.json` depends on `readdir`,
 *      so we make it explicit. Recovery from a cancelled pass-1: home
 *      screen still lists the slot (metadata is intact) and a second
 *      uninstall picks up cleanly because pass 1 is idempotent.
 *
 * `find -delete` prints every deleted path on stdout; we silence that
 * with `>/dev/null` so only stderr (real errors) reach the log.
 */
object RootfsCleaner {

    /**
     * Wipe [installDir] and everything inside it. Idempotent — a
     * missing dir is a successful wipe, a leftover-from-cancelled-pass-1
     * dir (rootfs gone, metadata still there) re-runs cleanly. Throws
     * [IOException] on the first sub-step failure; the caller is
     * expected to record `FAILED` state if the dir survives.
     */
    fun wipe(installDir: File, log: (String) -> Unit = {}) {
        if (!installDir.exists()) return
        val rootfsPath = File(installDir, "rootfs").absolutePath
        val installPath = installDir.absolutePath

        log("kill: chroot processes (root=$rootfsPath)")
        try {
            ProcessScanner.killAllInRootfs(
                rootfsPath = rootfsPath,
                installId = installDir.name,
                includeChroot = true,
                log = { log("kill: $it") },
            )
        } catch (t: Throwable) {
            // Don't abort — we're about to nuke the rootfs anyway and
            // the unmount step below catches any actual blockers.
            log("kill: warning, ${t.javaClass.simpleName}: ${t.message}")
        }

        log("unmount: $rootfsPath")
        val ur = ChrootMounter.unmount(rootfsPath)
        if (!ur.ok) {
            throw IOException("Unmount refused (active mounts):\n${ur.output}")
        }

        // Pass 1: rootfs only. The bulk of the deletion lives here. If
        // a cancel arrives we want to tip over before metadata.json
        // gets touched, so the slot still shows up on the home screen
        // and a second uninstall can finish the job.
        if (File(rootfsPath).exists()) {
            log("rm: rootfs subtree at $rootfsPath")
            // No per-line `onLine` — `find -delete` on an Arch rootfs
            // emits one error line per file held open / locked / on a
            // filesystem `-xdev` won't cross, which can be thousands
            // of lines on a cancel path. The full output is still
            // captured in `rfRes.output` and shown on failure.
            val rfRes = Su.run("find '$rootfsPath' -xdev -depth -delete >/dev/null")
            if (!rfRes.ok) {
                throw IOException("rootfs delete failed:\n${rfRes.output}")
            }
        }

        // Pass 2: explicit ordering — `enter.sh` first, then any
        // half-written `metadata.json.tmp`, then `metadata.json`,
        // then `rmdir`. `find -depth -delete` would leave readdir-
        // order between siblings undefined, which means a cancel
        // between two arbitrary unlinks could orphan the slot
        // (installDir present, metadata.json gone — invisible on the
        // home screen). Explicit order makes metadata.json the
        // second-to-last visible artefact, with only the empty dir
        // remaining and `rmdir` finishing the job near-atomically.
        log("rm: container at $installPath (enter.sh, metadata.json, rmdir)")
        val finalRes = Su.run(
            buildString {
                appendLine("rm -f '$installPath/enter.sh'")
                appendLine("rm -f '$installPath/metadata.json.tmp'")
                appendLine("rm -f '$installPath/metadata.json'")
                appendLine("rmdir '$installPath'")
            }
        )
        if (!finalRes.ok || installDir.exists()) {
            throw IOException("metadata delete failed:\n${finalRes.output}")
        }
    }

}
