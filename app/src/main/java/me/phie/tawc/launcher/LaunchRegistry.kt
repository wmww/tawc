package me.phie.tawc.launcher

import android.content.Context
import android.os.SystemClock
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.launch
import me.phie.tawc.PointerEmulation
import me.phie.tawc.compositor.CompositorService
import me.phie.tawc.compositor.NativeBridge
import java.io.File
import java.util.concurrent.ConcurrentHashMap
import kotlin.random.Random

/** Where a GUI launch from [EntryLauncher] is (notes/launcher.md, "Launch splash"). */
sealed interface LaunchState {
    /** Splash up, waiting for the window. */
    data object Waiting : LaunchState

    /** Exited 0 with nothing left in its session: likely handed off to a
     *  running instance. Still the splash, for a short grace period. */
    data object Handoff : LaunchState

    /** No window yet after the timeout; the program still runs. */
    data object TimedOut : LaunchState

    /** Exited without a window. */
    data class Exited(val code: Int) : LaunchState

    /** The program couldn't be started. */
    data class Failed(val message: String) : LaunchState

    /** Handoff grace ran out: close quietly with a toast. */
    data object Quit : LaunchState

    /** No host could be reserved: close the splash; the window opens in
     *  its own Activity as without a splash. */
    data object Detached : LaunchState

    /** A window was assigned to the splash task; waiting for its first frame. */
    data object Matched : LaunchState

    /** The window is on screen and the splash gone. */
    data object Shown : LaunchState
}

sealed interface LaunchEvent {
    /** [sessionAlive]: other processes of the launch's session still run. */
    data class Exited(val code: Int, val sessionAlive: Boolean) : LaunchEvent
    data class Failed(val message: String) : LaunchEvent
    data object Detached : LaunchEvent
    data object TimeoutOver : LaunchEvent
    data object GraceOver : LaunchEvent
    data object Matched : LaunchEvent
    data object Shown : LaunchEvent
}

/** The launch state machine. Pure; [LaunchRegistry] runs its timers. */
internal fun reduce(state: LaunchState, event: LaunchEvent): LaunchState {
    val showingLog = state is LaunchState.TimedOut || state is LaunchState.Exited || state is LaunchState.Failed
    val waiting = state == LaunchState.Waiting || state == LaunchState.Handoff
    return when (event) {
        is LaunchEvent.Exited -> when {
            // A launcher script that backgrounds the real program exits 0
            // with the program still in its session: keep waiting.
            event.code == 0 && event.sessionAlive &&
                (state == LaunchState.Waiting || state == LaunchState.TimedOut) -> state
            state == LaunchState.Waiting && event.code == 0 -> LaunchState.Handoff
            state == LaunchState.Waiting || state == LaunchState.TimedOut -> LaunchState.Exited(event.code)
            else -> state
        }
        is LaunchEvent.Failed -> if (waiting || state == LaunchState.TimedOut) LaunchState.Failed(event.message) else state
        LaunchEvent.Detached -> if (state == LaunchState.Waiting) LaunchState.Detached else state
        LaunchEvent.TimeoutOver -> if (state == LaunchState.Waiting) LaunchState.TimedOut else state
        LaunchEvent.GraceOver -> if (state == LaunchState.Handoff) LaunchState.Quit else state
        LaunchEvent.Matched -> if (waiting || showingLog) LaunchState.Matched else state
        LaunchEvent.Shown -> if (waiting || showingLog || state == LaunchState.Matched) LaunchState.Shown else state
    }
}

/** One GUI launch: its splash's content, state and captured output. */
class Launch internal constructor(
    /** Also the splash host's `ActivityId`. */
    val id: String,
    val desktopId: String,
    val name: String,
    val iconPath: String,
    val terminal: Boolean,
    internal val timeoutMs: Long,
    /** The entry's override, handed to the compositor at [LaunchRegistry.reserve]. */
    val pointerEmulation: PointerEmulation? = null,
) {
    private val _state = MutableStateFlow<LaunchState>(LaunchState.Waiting)
    val state: StateFlow<LaunchState> = _state

    /** Bumped on every captured line; read the text with [logText]. */
    private val _logVersion = MutableStateFlow(0L)
    val logVersion: StateFlow<Long> = _logVersion

    private val log = ArrayDeque<String>()
    private var logBytes = 0

    /** The splash Activity came up. */
    @Volatile var attached = false
    /** The splash's Android task, for tests; -1 until it is up. */
    @Volatile var taskId = -1

    internal var reserved = false
    internal var released = false
    internal var timer: Job? = null

    internal fun setState(state: LaunchState) {
        _state.value = state
    }

    /** One line of the program's stdout/stderr. Kept until a window
     *  matches; after that the output is drained and dropped. */
    fun appendLog(line: String) {
        val s = _state.value
        if (released || s == LaunchState.Matched || s == LaunchState.Shown) return
        synchronized(log) {
            log.addLast(line)
            logBytes += line.length + 1
            while (log.size > LOG_MAX_LINES || logBytes > LOG_MAX_BYTES) {
                logBytes -= log.removeFirst().length + 1
            }
        }
        _logVersion.value++
    }

    fun logText(): String = synchronized(log) { log.joinToString("\n") }

    internal fun dropLog() = synchronized(log) {
        log.clear()
        logBytes = 0
    }

    private companion object {
        const val LOG_MAX_LINES = 1000
        const val LOG_MAX_BYTES = 128 * 1024
    }
}

/**
 * Process-wide GUI launches by id, so the splash ([me.phie.tawc.compositor.CompositorActivity]
 * in launch mode) can find its record, and timers outlive configuration
 * changes. A record lives until its splash Activity is closed ([release]).
 */
object LaunchRegistry {
    private const val TAG = "tawc-launcher"

    /** No window by then: show the log. */
    const val DEFAULT_TIMEOUT_MS = 20_000L
    /** Exit 0 with no window: wait this long for a handed-off one. */
    private const val GRACE_MS = 5_000L
    private const val RUNNING_WAIT_MS = 5_000L

    private val launches = ConcurrentHashMap<String, Launch>()
    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Main.immediate)

    fun get(id: String): Launch? = launches[id]

    fun create(
        desktopId: String,
        name: String,
        iconPath: String,
        terminal: Boolean,
        timeoutMs: Long = DEFAULT_TIMEOUT_MS,
        pointerEmulation: PointerEmulation? = null,
    ): Launch {
        val id = "launch-%016x".format(Random.nextLong())
        val launch = Launch(id, desktopId, name, iconPath, terminal, timeoutMs, pointerEmulation)
        launches[id] = launch
        scope.launch { schedule(launch, LaunchState.Waiting) }
        return launch
    }

    /** Feed [event] to [id]'s state machine. Any thread. */
    fun dispatch(id: String, event: LaunchEvent) {
        scope.launch {
            val launch = launches[id] ?: return@launch
            val old = launch.state.value
            val new = reduce(old, event)
            if (new == old) return@launch
            launch.setState(new)
            if (new == LaunchState.Matched || new == LaunchState.Shown) launch.dropLog()
            schedule(launch, new)
        }
    }

    private fun schedule(launch: Launch, state: LaunchState) {
        launch.timer?.cancel()
        val (delayMs, event) = when (state) {
            LaunchState.Waiting -> launch.timeoutMs to LaunchEvent.TimeoutOver
            LaunchState.Handoff -> GRACE_MS to LaunchEvent.GraceOver
            else -> return
        }
        launch.timer = scope.launch {
            delay(delayMs)
            dispatch(launch.id, event)
        }
    }

    /**
     * Reserve the splash host for [launch]'s first window, starting the
     * compositor if needed. Blocks; call off the main thread. Returns the
     * activation token for the program, or null if no reservation could
     * be made (or the splash is already gone).
     */
    fun reserve(context: Context, launch: Launch): String? {
        CompositorService.ensureActivation(context)
        repeat(3) {
            if (launch.released) return null
            CompositorService.ensureRunning(context)
            val deadline = SystemClock.uptimeMillis() + RUNNING_WAIT_MS
            while (!NativeBridge.nativeIsCompositorRunning() && SystemClock.uptimeMillis() < deadline) {
                Thread.sleep(20)
            }
            val token = NativeBridge.nativeReserveLaunchHost(
                launch.id,
                launch.desktopId,
                launch.pointerEmulation?.ordinal ?: -1,
            )
            if (token != null) {
                val releasedMeanwhile = synchronized(launch) {
                    launch.reserved = true
                    launch.released
                }
                if (releasedMeanwhile) {
                    NativeBridge.nativeReleaseLaunchHost(launch.id)
                    return null
                }
                return token
            }
        }
        Log.w(TAG, "launch ${launch.id}: no host reserved")
        return null
    }

    /** The splash Activity closed: drop the record and the reservation.
     *  The program keeps running. */
    fun release(id: String) {
        val launch = launches.remove(id) ?: return
        val reserved = synchronized(launch) {
            launch.released = true
            launch.reserved
        }
        launch.timer?.cancel()
        launch.dropLog()
        if (reserved) NativeBridge.nativeReleaseLaunchHost(id)
    }

    /** Whether any process is still in session [sid]. */
    fun sessionAlive(sid: Int): Boolean {
        val procs = File("/proc").listFiles() ?: return false
        return procs.any { dir ->
            dir.name.all(Char::isDigit) && runCatching {
                val stat = File(dir, "stat").readText()
                // comm may hold spaces and parens; the session is 4th after it.
                stat.substring(stat.lastIndexOf(')') + 1).trim().split(' ')[3].toInt() == sid
            }.getOrDefault(false)
        }
    }
}
