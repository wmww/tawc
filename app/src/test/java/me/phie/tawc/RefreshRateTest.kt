package me.phie.tawc

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The cap rules the display row relies on, including the messy values
 * real panels report (59.94 Hz as 59940 mHz, duplicates per mode).
 */
class RefreshRateTest {
    private val s9 = listOf(120_000, 60_000, 30_000)

    @Test
    fun `max means the highest supported rate`() {
        assertEquals(120_000, RefreshRate.effectiveMhz(s9, RefreshRate.MAX_MHZ))
    }

    @Test
    fun `a cap picks the highest rate at or below it`() {
        assertEquals(60_000, RefreshRate.effectiveMhz(s9, 60_000))
        assertEquals(60_000, RefreshRate.effectiveMhz(s9, 90_000))
        assertEquals(30_000, RefreshRate.effectiveMhz(s9, 30_000))
    }

    @Test
    fun `a cap below every mode still asks for the slowest one`() {
        assertEquals(30_000, RefreshRate.effectiveMhz(s9, 24_000))
    }

    @Test
    fun `unsorted duplicates and odd rates are handled`() {
        val panel = listOf(59_940, 119_880, 59_940, 24_000)
        assertEquals(119_880, RefreshRate.effectiveMhz(panel, RefreshRate.MAX_MHZ))
        assertEquals(59_940, RefreshRate.effectiveMhz(panel, 60_000))
        assertEquals(24_000, RefreshRate.effectiveMhz(panel, 30_000))
    }

    @Test
    fun `implausible rates are dropped rather than clamped`() {
        val junk = listOf(0, 1, 1_000_000)
        assertNull(RefreshRate.effectiveMhz(junk, RefreshRate.MAX_MHZ))
        assertNull(RefreshRate.effectiveMhz(emptyList(), RefreshRate.MAX_MHZ))
        assertEquals(120_000, RefreshRate.effectiveMhz(s9 + 1_000_000, RefreshRate.MAX_MHZ))
    }

    @Test
    fun `the offered list is deduplicated and ascending`() {
        assertEquals(listOf(30_000, 60_000, 120_000), RefreshRate.usableMhz(s9))
        assertEquals(listOf(24_000, 59_940, 119_880), RefreshRate.usableMhz(listOf(119_880, 24_000, 59_940, 24_000)))
        assertEquals(emptyList<Int>(), RefreshRate.usableMhz(listOf(0, 1_000_000)))
    }

    @Test
    fun `caps are sanitized to max or a plausible rate`() {
        assertEquals(RefreshRate.MAX_MHZ, RefreshRate.sanitizeCapMhz(0))
        assertEquals(RefreshRate.MAX_MHZ, RefreshRate.sanitizeCapMhz(-5))
        assertEquals(RefreshRate.MAX_MHZ, RefreshRate.sanitizeCapMhz(5_000))
        assertEquals(60_000, RefreshRate.sanitizeCapMhz(60_000))
        assertEquals(240_000, RefreshRate.sanitizeCapMhz(999_000))
    }

    @Test
    fun `labels drop the fraction when there is none`() {
        assertEquals("120 Hz", RefreshRate.formatMhz(120_000))
        assertEquals("59.9 Hz", RefreshRate.formatMhz(59_940))
    }
}
