package me.phie.tawc.install

import android.content.Context
import java.io.File

/**
 * Real-chroot implementation of [InstallationMethod]: bind mounts plus
 * `chroot(2)`, all under Magisk's `su`. The mount logic and `enter.sh`
 * generator live in [ChrootMounter]; this object just routes interface
 * calls there.
 *
 * Sole consumer of [Su.run] in the install package — keeping the
 * `requiresRoot=true` quarantine here makes the rest of the install
 * pipeline method-agnostic (and in particular keeps [ProotMethod] from
 * accidentally inheriting root assumptions).
 */
object ChrootMethod : InstallationMethod {
    const val KEY = "chroot"
    override val key: String = KEY
    override val displayName: String = "chroot (root)"
    override val requiresRoot: Boolean = true

    override fun isAvailable(context: Context): Boolean = Su.rootAvailable()

    override fun runOutside(
        script: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        val r = Su.run(script, onLine = onLine)
        return MethodResult(r.exitCode, r.output)
    }

    /**
     * Start a chroot subprocess running [command] inside [rootfs] under
     * `su`. The bind-mount + chroot setup script is piped to su via
     * stdin; the subsequent in-rootfs bash inherits that stdin pipe,
     * but by the time bash takes over we've already written everything
     * we want and the shell has consumed it (the `exec setsid chroot`
     * line replaces the shell, leaving any further stdin bytes for
     * bash).
     *
     * `setsid` upholds the chroot-session invariant
     * (notes/chroot-sessions.md): every chroot invocation runs in its
     * own session.
     */
    override fun startInside(rootfs: String, command: String?): Process {
        // Magisk's su inherits the calling process's mount namespace,
        // so we wrap with `unshare -m` so any leaked binds go away when
        // the script exits. (See [Su.run]'s docstring for context.)
        val proc = ProcessBuilder(listOf("su", "-c", "exec unshare -m -- /system/bin/sh"))
            .start()
        val script = buildString {
            appendLine("set -eu")
            appendLine(ChrootMounter.mountScript(rootfs))
            // Quote rootfs and (if present) the user command into the
            // script. Both go through shellQuote so paths with quotes
            // can't break out.
            val rootfsQ = "'" + rootfs.replace("'", "'\\''") + "'"
            if (command != null) {
                val cmdQ = "'" + command.replace("'", "'\\''") + "'"
                appendLine("exec setsid chroot $rootfsQ /bin/bash -lc $cmdQ")
            } else {
                appendLine("exec setsid chroot $rootfsQ /bin/bash -l")
            }
        }
        // Write+flush; intentionally don't close (exec replaces the
        // shell so further stdin bytes flow into the in-rootfs bash).
        val w = proc.outputStream.bufferedWriter()
        w.write(script); w.write("\n"); w.flush()
        return proc
    }

    /** Delegates to [Archive.extractAsRoot] (the historical path). */
    override fun extractBootstrap(
        tarball: File,
        rootfs: String,
        format: BootstrapFormat,
        stripPrefix: String?,
        tempFifo: File,
        onLine: (String) -> Unit,
    ) {
        Archive.extractAsRoot(
            tarball = tarball,
            destDir = rootfs,
            tempFifo = tempFifo,
            stripPrefix = stripPrefix,
            onLine = onLine,
        )
    }

    /** Delegates to [RootfsCleaner.wipe] (the historical path). */
    override fun wipe(installDir: File, log: (String) -> Unit) {
        RootfsCleaner.wipe(installDir, log)
    }
}
