package me.phie.tawc.install

import android.util.Log
import java.io.IOException
import java.util.concurrent.TimeUnit

/**
 * Run shell commands as root via Magisk's `su`.
 *
 * The script is piped to `su` via stdin (rather than passed as `-c <arg>`)
 * so we never have to escape quoting through an outer shell. stderr is
 * folded into stdout. Output is delivered line-by-line to a callback so
 * long-running commands (like `pacman -Syu`) can stream progress to the UI.
 *
 * Magisk grants `su` per-app via a policy in `/data/adb/magisk.db`. On a
 * real device the user accepts a Magisk prompt the first time the app
 * calls su; on the emulator we pre-grant by uid (see
 * notes/installation.md and notes/emulator.md).
 */
object Su {
    private const val TAG = "tawc-install"

    /**
     * Result of running a shell script via su.
     *  - [exitCode] is the script's final exit status (0 on success).
     *  - [output] is the full combined stdout+stderr (also delivered line-by-line
     *    to the callback). Truncated to 256 KiB to avoid blowing up logs.
     */
    data class Result(val exitCode: Int, val output: String) {
        val ok: Boolean get() = exitCode == 0
    }

    /** Quick check that `su` is available and grants uid 0. */
    fun rootAvailable(): Boolean {
        return try {
            val r = run("id -u", timeoutSeconds = 10)
            r.ok && r.output.trim() == "0"
        } catch (e: IOException) {
            false
        }
    }

    /**
     * Run [script] (a `/bin/sh` snippet) as root.
     *
     * Each output line is reported via [onLine] (if provided) before being
     * appended to the captured output buffer. The script runs with `set -eu`
     * prepended so the first failing command aborts.
     *
     * Magisk runs each `su` invocation inside a private mount namespace
     * (`unshare(CLONE_NEWNS)`), so any bind mounts performed by [script]
     * vanish when the call returns. Callers that need mounts must keep
     * mount + chroot in the same script — the canonical entry point is
     * `<installation-dir>/enter.sh`, rendered by [ChrootMounter.enterScript].
     *
     * Set [mountMaster] to launch via `su -mm` (Magisk's "mount master"
     * mode) — joins the global mount namespace instead of a private one,
     * so `mount` / `umount` calls affect every other process on the
     * device. Used by [ChrootMounter.unmount] to clean up bind mounts
     * leaked into the global namespace by host-side `tawc-chroot-run`
     * invocations (which inherit the adb shell's mount-master `su`).
     */
    fun run(
        script: String,
        timeoutSeconds: Long = 0,
        mountMaster: Boolean = false,
        onLine: ((String) -> Unit)? = null,
    ): Result {
        val cmd = if (mountMaster) listOf("su", "-mm") else listOf("su")
        val pb = ProcessBuilder(cmd).redirectErrorStream(true)
        // Magisk's su inherits a sane PATH itself; nothing to do here.
        val proc = pb.start()
        proc.outputStream.bufferedWriter().use { w ->
            w.write("set -eu\n")
            w.write(script)
            // Trailing newline so the last line is processed even without one in [script].
            w.write("\n")
        }

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
                Log.w(TAG, "su stdout reader: $e")
            }
        }.also { it.start() }

        val finished = if (timeoutSeconds > 0) {
            proc.waitFor(timeoutSeconds, TimeUnit.SECONDS)
        } else {
            proc.waitFor(); true
        }
        if (!finished) {
            proc.destroyForcibly()
            readerThread.join(2000)
            throw IOException("su script timed out after ${timeoutSeconds}s")
        }
        readerThread.join(2000)
        return Result(proc.exitValue(), sb.toString())
    }
}
