package me.phie.tawc.install

import android.util.Log
import java.io.IOException

/**
 * Run shell scripts as the app uid — the rootless sibling of [Su].
 * Same shape: script piped via stdin (never quote-escaped through an
 * outer shell), stderr folded into stdout, output capped at 256 KiB,
 * lines streamed through an optional callback.
 *
 * No `set -eu` is prepended (unlike [Su.run]): callers' scripts
 * manage their own exit status — some deliberately end in `; exit 0`,
 * others want the last command's code as the verdict. Prepend it
 * yourself for multi-step abort-on-failure scripts (see
 * [ProotMethod.runOutside]).
 */
object Sh {
    private const val TAG = "tawc-install"

    fun run(
        script: String,
        onLine: ((String) -> Unit)? = null,
        env: Map<String, String> = emptyMap(),
    ): MethodResult {
        val pb = ProcessBuilder(listOf("/system/bin/sh")).redirectErrorStream(true)
        pb.environment().putAll(env)
        val proc = pb.start()
        proc.outputStream.bufferedWriter().use { w ->
            w.write(script)
            w.write("\n")
        }
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
                Log.w(TAG, "sh stdout reader: $e")
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

    /** Quote [s] for inclusion in a shell script as one word. */
    fun quote(s: String): String = "'" + s.replace("'", "'\\''") + "'"
}
