package me.phie.tawc.install.util

/**
 * Format a byte count as a short human-readable string (`"1.2 MiB"`,
 * `"512 B"`). Used by download progress messages.
 */
object HumanSize {
    fun format(bytes: Long): String {
        if (bytes < 1024) return "${bytes} B"
        val units = arrayOf("KiB", "MiB", "GiB", "TiB")
        var v = bytes / 1024.0
        var i = 0
        while (v >= 1024 && i < units.size - 1) { v /= 1024; i++ }
        return String.format("%.1f %s", v, units[i])
    }
}
