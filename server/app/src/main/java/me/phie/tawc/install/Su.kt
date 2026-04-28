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

    @Volatile private var cachedRootAvailable: Boolean? = null

    /**
     * Quick check that `su` is available and grants uid 0. The result
     * is memoised for the lifetime of the process — Magisk's policy
     * doesn't change without an app cold-start, and the underlying
     * `su id -u` probe blocks up to 10s, which would ANR if called
     * repeatedly from UI onCreate paths (radio picker, button gates,
     * uninstall preflight). The first call pays the cost; every later
     * call is a volatile read.
     */
    fun rootAvailable(): Boolean {
        cachedRootAvailable?.let { return it }
        val result = try {
            val r = run("id -u", timeoutSeconds = 10)
            r.ok && r.output.trim() == "0"
        } catch (e: IOException) {
            false
        }
        cachedRootAvailable = result
        return result
    }

    /**
     * Run [script] (a `/bin/sh` snippet) as root.
     *
     * Each output line is reported via [onLine] (if provided) before being
     * appended to the captured output buffer. The script runs with `set -eu`
     * prepended so the first failing command aborts.
     *
     * Magisk's `su` inherits the **calling** process's mount namespace
     * by default — bind mounts performed inside one [run] would
     * therefore persist into the app's namespace and pollute every
     * later call (the rootfs's recursive `/data/data/<pkg>` self-bind
     * then trips `find -xdev` with "loop detected" during uninstall).
     * To keep each invocation isolated we wrap the non-`mountMaster`
     * path in `unshare -m`, which gives the script its own private
     * mount namespace that's torn down when the script exits.
     *
     * Set [mountMaster] to launch via `su -mm` (Magisk's "mount master"
     * mode) — joins the global (init) mount namespace, so `mount` /
     * `umount` calls affect every other process on the device. Used by
     * [ChrootMounter.unmount] to clean up bind mounts leaked into the
     * global namespace by host-side `tawc-chroot-run` invocations
     * (which inherit the adb shell's mount-master `su`). We deliberately
     * do *not* unshare the `-mm` path: the global namespace is the whole
     * point of `-mm`.
     */
    fun run(
        script: String,
        timeoutSeconds: Long = 0,
        mountMaster: Boolean = false,
        onLine: ((String) -> Unit)? = null,
    ): Result {
        // su parses -c <arg> and feeds <arg> to its shell; the inner
        // `exec unshare -m -- /system/bin/sh` replaces that shell with
        // sh-in-fresh-mount-namespace, which then reads our piped
        // script from stdin. This is the only safe way to ensure no
        // bind mount escapes the call.
        val cmd = if (mountMaster) listOf("su", "-mm")
                  else listOf("su", "-c", "exec unshare -m -- /system/bin/sh")
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
        }.also { it.isDaemon = true; it.start() }

        try {
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
        } catch (e: InterruptedException) {
            // Caller (typically `runInterruptible`) cancelled. Kill the
            // subprocess so a long-running `du`/`pacman` doesn't outlive
            // the request, then re-raise so the coroutine machinery
            // translates this back into CancellationException.
            proc.destroyForcibly()
            readerThread.join(2000)
            Thread.currentThread().interrupt()
            throw e
        }
        readerThread.join(2000)
        return Result(proc.exitValue(), sb.toString())
    }
}
