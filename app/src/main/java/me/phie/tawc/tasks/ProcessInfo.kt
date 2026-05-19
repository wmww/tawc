package me.phie.tawc.tasks

/**
 * One running guest process discovered by [ProcessScanner].
 *
 * "Owner" is the install whose rootfs the process appears to be using,
 * determined from kernel-side `/proc` paths (cheap links first, plus
 * executable maps for tawcroot; see [AppUidProcfsScanner] /
 * [SuProcfsScanner] for the exact predicate). [ownerInstallId] is
 * `null` for *orphan* processes —
 * processes whose paths point into a `<distros>/<id>/rootfs` tree that
 * no longer has an installation record (e.g. an `--kill-on-exit` leak
 * that survived an uninstall). The id is preserved as
 * [orphanRootfsId] so the UI can show "(uninstalled: <id>)" rather
 * than just "orphan".
 *
 * [requiresSu] tells [ProcessScanner.stop] which kill path to use:
 * `false` for app-uid processes (use Os.kill directly), `true` for
 * root-owned chroot processes (must shell out via [Su]). Set only by
 * [SuProcfsScanner]; will be removed alongside that file when chroot
 * support is dropped from production builds.
 */
data class ProcessInfo(
    val pid: Int,
    val parentPid: Int,
    val ownerInstallId: String?,
    val orphanRootfsId: String?,
    val comm: String,
    val cmdline: String,
    val cwd: String,
    val guestCommand: String,
    val displayCommand: String,
    val requiresSu: Boolean,
)
