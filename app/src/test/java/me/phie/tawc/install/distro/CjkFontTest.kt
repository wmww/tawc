package me.phie.tawc.install.distro

import me.phie.tawc.install.Installation
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The China-preset reminder in the settings row says "install a CJK
 * font" and hands the user their distro's own command, so both halves
 * of that are pinned here: the command per package-manager family, and
 * the fact that a preset labelled China really carries
 * [CHINA_MIRROR_REGION_ID] (a China preset under another id would
 * silently never fire the hint).
 */
class CjkFontTest {
    @Test
    fun `cjk command follows the package manager family`() {
        val byKey = DistroRegistry.all.associate { it.key to cjkFontCommand(it) }
        assertEquals("pacman -S noto-fonts-cjk", byKey[Installation.DISTRO_ARCH])
        assertEquals("pacman -S noto-fonts-cjk", byKey[Installation.DISTRO_MANJARO])
        assertEquals("apt-get install -y fonts-noto-cjk", byKey[Installation.DISTRO_DEBIAN_SID])
        assertEquals("xbps-install -S noto-fonts-cjk", byKey[Installation.DISTRO_VOID])
    }

    @Test
    fun `china presets use the id the hint checks`() {
        val lists = listOf(
            MirrorRegions.archLinuxArm,
            MirrorRegions.archLinux,
            MirrorRegions.manjaroArm,
            MirrorRegions.debian,
            MirrorRegions.voidLinux,
            MirrorRegions.ubuntu,
        )
        val withChina = lists.filter { list -> list.any { it.label == "China" } }
        assertTrue("no China preset anywhere", withChina.isNotEmpty())
        for (list in withChina) {
            assertTrue(
                "a China preset does not use '$CHINA_MIRROR_REGION_ID'",
                list.any { it.id == CHINA_MIRROR_REGION_ID },
            )
        }
    }
}
