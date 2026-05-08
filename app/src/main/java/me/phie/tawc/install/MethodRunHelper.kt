package me.phie.tawc.install

import android.util.Log
import java.io.IOException

/**
 * Shared default body for [InstallationMethod.runInside]: starts the
 * process via [InstallationMethod.startInside], relays stdout/stderr
 * line-by-line through `onLine`, waits for exit. Kept here so each
 * method's runInside is identical and the per-method file only owns
 * the `startInside` implementation that actually differs.
 *
 * stderr is folded into the same line stream because every existing
 * caller (Installer pipeline, ArchPacmanCommon) wants both, and the
 * old `runInside` returned a single combined buffer.
 */
internal object MethodRunHelper {
    private const val TAG = "tawc-install"
    private const val OUTPUT_CAP_BYTES = 256 * 1024

    fun runInside(
        method: InstallationMethod,
        rootfs: String,
        command: String,
        onLine: ((String) -> Unit)?,
    ): MethodResult {
        val proc = method.startInside(rootfs, command)
        // We don't redirectErrorStream up at startInside (the broker
        // path needs them split into separate frames), so collect both
        // here and merge into one ordered-by-arrival line stream.
        val sb = StringBuilder()
        val collect: (String) -> Unit = { line ->
            synchronized(sb) {
                if (sb.length < OUTPUT_CAP_BYTES) {
                    if (sb.isNotEmpty()) sb.append('\n')
                    sb.append(line)
                }
            }
            onLine?.invoke(line)
        }
        val outT = Thread {
            try {
                proc.inputStream.bufferedReader().forEachLine(collect)
            } catch (e: IOException) {
                Log.w(TAG, "stdout reader: $e")
            }
        }.also { it.isDaemon = true; it.start() }
        val errT = Thread {
            try {
                proc.errorStream.bufferedReader().forEachLine(collect)
            } catch (e: IOException) {
                Log.w(TAG, "stderr reader: $e")
            }
        }.also { it.isDaemon = true; it.start() }
        try {
            proc.waitFor()
        } catch (e: InterruptedException) {
            proc.destroyForcibly()
            outT.join(2000); errT.join(2000)
            Thread.currentThread().interrupt()
            throw e
        }
        outT.join(2000); errT.join(2000)
        return MethodResult(proc.exitValue(), sb.toString())
    }
}
