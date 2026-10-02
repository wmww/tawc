package me.phie.tawc.home

import org.junit.Assert.assertEquals
import org.junit.Test

/**
 * Shapes the dashboard card prints. Pure formatting, so the exact strings
 * the card shows are pinned here rather than eyeballed on a device.
 */
class DashboardStatsTest {

    @Test
    fun memoryReadsAsGigabytesAboveTheUnit() {
        assertEquals("2.4 GB", DashboardStats.formatBytes(2_576_980_377L))
        assertEquals("1.0 GB", DashboardStats.formatBytes(1L shl 30))
        assertEquals("16.0 GB", DashboardStats.formatBytes(16L shl 30))
    }

    @Test
    fun memoryReadsAsWholeMegabytesBelowIt() {
        assertEquals("512 MB", DashboardStats.formatBytes(512L shl 20))
        assertEquals("0 MB", DashboardStats.formatBytes(0))
        assertEquals("1023 MB", DashboardStats.formatBytes((1L shl 30) - 1))
    }

    @Test
    fun uptimeKeepsTheTwoCoarsestUnits() {
        assertEquals("2h 14m", DashboardStats.formatUptime(2 * 3_600_000L + 14 * 60_000L))
        assertEquals("3d 4h", DashboardStats.formatUptime((3 * 24L + 4) * 3_600_000L))
        assertEquals("14m", DashboardStats.formatUptime(14 * 60_000L))
    }

    @Test
    fun uptimeNeverPrintsUnitsItDoesNotNeed() {
        assertEquals("0m", DashboardStats.formatUptime(0))
        assertEquals("0m", DashboardStats.formatUptime(59_999))
        assertEquals("1m", DashboardStats.formatUptime(60_000))
        assertEquals("1h 0m", DashboardStats.formatUptime(3_600_000))
        assertEquals("1d 0h", DashboardStats.formatUptime(24 * 3_600_000L))
    }
}
