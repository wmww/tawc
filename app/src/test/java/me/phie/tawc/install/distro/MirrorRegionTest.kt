package me.phie.tawc.install.distro

import me.phie.tawc.install.distro.apt.AptCommon
import me.phie.tawc.install.distro.arch.ArchLinuxArm
import me.phie.tawc.install.distro.arch.ArchLinuxX86_64
import me.phie.tawc.install.distro.debian.DebianSidAarch64
import me.phie.tawc.install.distro.manjaro.ManjaroArm
import me.phie.tawc.install.distro.voidlinux.VoidCommon
import me.phie.tawc.install.distro.voidlinux.VoidLinuxAarch64
import me.phie.tawc.install.distro.voidlinux.VoidLinuxX86_64
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * Mirror presets and their rendering.
 *
 * The load-bearing case is [defaultsAreUnchanged]: an install whose user
 * never opened the setting must configure exactly the source it
 * configured before the setting existed — byte for byte, for every
 * distro family. The rest pins the contract each family's config format
 * has to satisfy.
 */
class MirrorRegionTest {

    @Test
    fun defaultsAreUnchanged() {
        // Verbatim copies of each distro's pre-setting constant.
        assertEquals(
            listOf(
                "Server = https://fl.us.mirror.archlinuxarm.org/\$arch/\$repo",
                "Server = https://ca.us.mirror.archlinuxarm.org/\$arch/\$repo",
                "Server = http://mirror.archlinuxarm.org/\$arch/\$repo",
                "Server = http://nj.us.mirror.archlinuxarm.org/\$arch/\$repo",
                "Server = http://de.mirror.archlinuxarm.org/\$arch/\$repo",
                "Server = http://fr.mirror.archlinuxarm.org/\$arch/\$repo",
            ).joinToString("\n"),
            ArchLinuxArm.mirrorConfig(null),
        )
        assertEquals(
            "Server = https://geo.mirror.pkgbuild.com/\$repo/os/\$arch",
            ArchLinuxX86_64.mirrorConfig(null),
        )
        assertEquals(
            listOf(
                "Server = https://mirror.alwyzon.net/manjaro/arm-testing/\$repo/\$arch",
                "Server = https://manjaro.repo.cure.edu.uy/arm-testing/\$repo/\$arch",
                "Server = https://mirror.futureweb.be/manjaro/arm-testing/\$repo/\$arch",
                "Server = https://ftp.linux.org.tr/manjaro/arm-testing/\$repo/\$arch",
                "Server = https://mirrors.dotsrc.org/manjaro/arm-testing/\$repo/\$arch",
            ).joinToString("\n"),
            ManjaroArm.mirrorConfig(null),
        )
        // Plain http, as shipped upstream today (apt verifies package
        // signatures against the shipped keyring regardless).
        assertEquals("http://deb.debian.org/debian", DebianSidAarch64.mirrorConfig(null))
        assertEquals(
            listOf("https://repo-fastly.voidlinux.org/current/aarch64"),
            VoidLinuxAarch64.mirrorConfig(null),
        )
        assertEquals(
            listOf("https://repo-fastly.voidlinux.org/current"),
            VoidLinuxX86_64.mirrorConfig(null),
        )
    }

    @Test
    fun unknownRegionFallsBackToTheDefault() {
        // Metadata written by a newer build (or a preset we removed) must
        // configure the default source, not fail the install.
        for (distro in DistroRegistry.all) {
            assertNull("${distro.key}/${distro.androidAbi}", distro.resolveMirrorRegion(null))
            assertNull("${distro.key}/${distro.androidAbi}", distro.resolveMirrorRegion("no-such-region"))
        }
    }

    @Test
    fun knownRegionResolves() {
        assertSame(
            MirrorRegions.archLinuxArm.first { it.id == "cn" },
            ArchLinuxArm.resolveMirrorRegion("cn"),
        )
        assertSame(
            MirrorRegions.debian.first { it.id == "cn" },
            DebianSidAarch64.resolveMirrorRegion("cn"),
        )
        assertSame(
            MirrorRegions.voidLinux.first { it.id == "cn" },
            VoidLinuxAarch64.resolveMirrorRegion("cn"),
        )
        assertSame(
            MirrorRegions.manjaroArm.first { it.id == "at" },
            ManjaroArm.resolveMirrorRegion("at"),
        )
    }

    /** pacman families: `Server = <base><suffix>` lines, in order. */
    @Test
    fun pacmanFamiliesRenderServerLines() {
        val region = MirrorRegion("test", "Test", listOf("https://a.example", "https://b.example"))
        assertEquals(
            "Server = https://a.example/\$arch/\$repo\nServer = https://b.example/\$arch/\$repo",
            ArchLinuxArm.mirrorConfig(region),
        )
        assertEquals(
            "Server = https://a.example/\$repo/os/\$arch\nServer = https://b.example/\$repo/os/\$arch",
            ArchLinuxX86_64.mirrorConfig(region),
        )
        // Manjaro's suffix is the plain repo path; the `arm-testing`
        // release channel is part of each mirror base (and of the
        // presets), so a region swap cannot move a user off it.
        assertEquals(
            "Server = https://a.example/\$repo/\$arch\nServer = https://b.example/\$repo/\$arch",
            ManjaroArm.mirrorConfig(region),
        )
        for (preset in MirrorRegions.manjaroArm) {
            for (line in ManjaroArm.mirrorConfig(preset).lineSequence()) {
                assertTrue(line, line.contains("/arm-testing/\$repo/\$arch"))
            }
        }
    }

    /** apt takes one archive root per source; the first base wins. */
    @Test
    fun debianRendersTheFirstBaseAsTheArchiveRoot() {
        val region = MirrorRegion("test", "Test", listOf("https://a.example/debian", "https://b.example/debian"))
        assertEquals("https://a.example/debian", DebianSidAarch64.mirrorConfig(region))
        val body = AptCommon.sourcesBody("sid", DebianSidAarch64.mirrorConfig(region), "/keyring.pgp")
        assertTrue(body, body.contains("URIs: https://a.example/debian"))
        assertTrue(body, body.contains("Suites: sid"))
        assertTrue(body, body.contains("Signed-By: /keyring.pgp"))
    }

    @Test
    fun voidRendersRepositoryLinesPerMirror() {
        val region = MirrorRegion("test", "Test", listOf("https://a.example/void", "https://b.example/void"))
        assertEquals(
            listOf("https://a.example/void/current/aarch64", "https://b.example/void/current/aarch64"),
            VoidLinuxAarch64.mirrorConfig(region),
        )
        assertEquals(
            listOf("https://a.example/void/current", "https://b.example/void/current"),
            VoidLinuxX86_64.mirrorConfig(region),
        )
        assertEquals(
            "repository=https://a.example/void/current/aarch64\n" +
                "repository=https://b.example/void/current/aarch64",
            VoidCommon.repositoryConf(VoidLinuxAarch64.mirrorConfig(region)),
        )
    }

    @Test
    fun presetsAreWellFormed() {
        for ((name, regions) in listOf(
            "archLinuxArm" to MirrorRegions.archLinuxArm,
            "archLinux" to MirrorRegions.archLinux,
            "manjaroArm" to MirrorRegions.manjaroArm,
            "debian" to MirrorRegions.debian,
            "voidLinux" to MirrorRegions.voidLinux,
            "ubuntu" to MirrorRegions.ubuntu,
        )) {
            assertTrue("$name has no presets", regions.isNotEmpty())
            assertEquals(
                "$name has duplicate ids",
                regions.size,
                regions.map { it.id }.toSet().size,
            )
            for (region in regions) {
                assertTrue("$name/${region.id} has a blank label", region.label.isNotBlank())
                assertTrue("$name/${region.id} has no servers", region.servers.isNotEmpty())
                for (server in region.servers) {
                    // A base URL with no scheme or a trailing slash would
                    // only surface as a 404 on-device.
                    assertTrue(
                        "$name/${region.id}: bad server $server",
                        server.startsWith("https://") || server.startsWith("http://"),
                    )
                    assertTrue(
                        "$name/${region.id}: trailing slash in $server",
                        !server.endsWith("/"),
                    )
                }
            }
        }
    }

    /**
     * Every shipped distro offers presets, and they are its own table.
     *
     * Ubuntu is the exception: its archives are per-architecture (arm64
     * lives on `ports`, amd64 on the main archive) and
     * [MirrorRegions.ubuntu] covers arm64 only, so its picker stays
     * hidden until an amd64 table exists. The key is a literal because
     * the Ubuntu flavour lands in its own change.
     */
    @Test
    fun everyDistroOffersItsOwnPresets() {
        for (distro in DistroRegistry.all) {
            if (distro.key == "ubuntu") continue
            assertTrue(
                "${distro.key}/${distro.androidAbi} offers no mirror presets",
                distro.mirrorRegions.isNotEmpty(),
            )
            assertTrue(
                "${distro.key}/${distro.androidAbi} presets are blank",
                distro.mirrorRegions.all { it.label.isNotBlank() && it.servers.isNotEmpty() },
            )
        }
        assertEquals(MirrorRegions.manjaroArm, ManjaroArm.mirrorRegions)
        assertEquals(MirrorRegions.debian, DebianSidAarch64.mirrorRegions)
        assertEquals(MirrorRegions.voidLinux, VoidLinuxAarch64.mirrorRegions)
    }
}
