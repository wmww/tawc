package me.phie.tawc.install

import android.content.Context
import android.util.Base64
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
     * Refresh `enter.sh` to the latest [ChrootMounter.enterScript]
     * output and exec it via `su` with a base64-encoded command.
     */
    override fun runInside(
        rootfs: String,
        command: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        val enterFile = File(File(rootfs).parentFile, "enter.sh")
        enterFile.writeText(ChrootMounter.enterScript(rootfs))
        enterFile.setExecutable(true, false)
        val cmdB64 = Base64.encodeToString(command.toByteArray(Charsets.UTF_8), Base64.NO_WRAP)
        val r = Su.run("exec '${enterFile.absolutePath}' $cmdB64", onLine = onLine)
        return MethodResult(r.exitCode, r.output)
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

    override fun enterScript(context: Context, rootfs: String): String =
        ChrootMounter.enterScript(rootfs)
}
