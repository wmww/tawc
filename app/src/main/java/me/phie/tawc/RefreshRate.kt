package me.phie.tawc

import android.content.Context
import android.hardware.display.DisplayManager
import android.view.Display
import kotlin.math.roundToInt

/**
 * Refresh-rate cap logic behind the display settings row.
 *
 * Kept pure (no `Display`, no `Context`) so the selection rules are
 * unit-testable without a device: everything here is **mHz**
 * (60000 = 60 Hz), which is the unit `Settings`, `wl_output.mode` and
 * the compositor use. Android's own API is in Hz and converts at the
 * edge.
 *
 * A high-refresh panel only helps when both halves agree: the Activity
 * has to ask the system for the rate (otherwise it sits at the panel
 * default or drops to 60 when idle), and the compositor has to advertise
 * it, or clients that pace themselves by the output (WebRender, games,
 * `wl_surface.frame` callbacks) keep rendering at 60.
 */
internal object RefreshRate {
    /** Cap meaning "whatever the panel can do" — the `Settings` default. */
    const val MAX_MHZ = 0

    /**
     * Plausibility window for a rate that came from `Display` or from a
     * prefs file. Values outside it are dropped instead of clamped: a
     * display that claims 1 Hz or 10 kHz is a bug we should not encode
     * into `wl_output.mode`.
     */
    private const val MIN_PLAUSIBLE_MHZ = 10_000
    private const val MAX_PLAUSIBLE_MHZ = 240_000

    /**
     * Rate to request from Android and to advertise to clients: the
     * highest supported rate at or below [capMhz], the highest supported
     * rate for [MAX_MHZ], and the lowest supported rate when the cap sits
     * below every mode (asking for something is better than silently
     * ignoring the setting).
     *
     * Null when [supportedMhz] holds nothing plausible — the caller then
     * leaves the panel default alone.
     */
    fun effectiveMhz(supportedMhz: List<Int>, capMhz: Int): Int? {
        val usable = usableMhz(supportedMhz)
        if (usable.isEmpty()) return null
        if (capMhz < MIN_PLAUSIBLE_MHZ) return usable.last()
        return usable.lastOrNull { it <= capMhz } ?: usable.first()
    }

    /**
     * The rates worth offering a user: plausible, de-duplicated (a panel
     * reports one entry per mode) and ascending, so `last()` is the panel
     * maximum the "Max" entry means.
     */
    fun usableMhz(supportedMhz: List<Int>): List<Int> = supportedMhz
        .filter { it in MIN_PLAUSIBLE_MHZ..MAX_PLAUSIBLE_MHZ }
        .distinct()
        .sorted()

    /** Cap values a settings row may persist: [MAX_MHZ] or a plausible rate. */
    fun sanitizeCapMhz(mhz: Int): Int =
        if (mhz < MIN_PLAUSIBLE_MHZ) MAX_MHZ else mhz.coerceAtMost(MAX_PLAUSIBLE_MHZ)

    /** "120 Hz", or "59.9 Hz" for the odd panel. */
    fun formatMhz(mhz: Int): String {
        val hz = mhz / 1000.0
        return if (hz == hz.toInt().toDouble()) {
            "${hz.toInt()} Hz"
        } else {
            String.format(java.util.Locale.ROOT, "%.1f Hz", hz)
        }
    }
}

/**
 * Every refresh rate the default display reports, in mHz, unsorted and
 * possibly duplicated (one entry per mode).
 *
 * Android-side half of [RefreshRate], which stays pure so the selection
 * rules are testable off-device. An empty list means "no usable display"
 * and makes every caller fall back to the panel default.
 */
internal fun displayRefreshRatesMhz(context: Context): List<Int> {
    val display = context.getSystemService(DisplayManager::class.java)
        ?.getDisplay(Display.DEFAULT_DISPLAY)
        ?: return emptyList()
    return display.supportedModes.map { (it.refreshRate * 1000f).roundToInt() }
}
