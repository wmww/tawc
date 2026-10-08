package me.phie.tawc.install

import me.phie.tawc.install.util.FsStat
import org.apache.commons.compress.archivers.tar.TarArchiveEntry
import org.apache.commons.compress.archivers.tar.TarArchiveOutputStream
import org.apache.commons.compress.archivers.tar.TarConstants
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Assert.fail
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.io.IOException
import java.nio.file.Files

/**
 * Distro export/import format ([DistroArchive], [DistroExporter.ArchiveWriter],
 * [DistroImporter.Reader]) on a real temp tree: round trip, exclusion
 * rules, the trailer check, and hostile archives. Archives are
 * uncompressed here (no host zstd-jni lib); production wraps zstd.
 */
class DistroArchiveTest {

    @get:Rule
    val tmp = TemporaryFolder()

    private val meta = Installation(
        id = "src",
        label = "My Arch",
        distro = "arch",
        arch = "arm64-v8a",
        method = "tawcroot",
        installedAtMillis = 1_000L,
        sourceUrl = "https://example/boot.tar.gz",
        tawcStamp = "stamp-1",
        tawcInstalls = listOf(TawcInstall("x", "/usr/local/bin/ando", TawcInstall.Type.COPY)),
        externalBinds = listOf(
            ExternalBind("/storage/emulated/0", "/home/android"),
            ExternalBind("/", "/android", readOnly = true),
        ),
        legacyHiddenDesktopIds = listOf("htop"),
        andoEnabled = true,
    )

    private fun manifest() = DistroArchive.Manifest(
        format = DistroArchive.FORMAT, createdAtMillis = 5_000L, appVersionName = "4",
        appVersionCode = 4, sourcePackage = "me.phie.tawc", id = "src", label = "My Arch",
        distro = "arch", arch = "arm64-v8a", method = "tawcroot", uncompressedBytes = 0, entries = 0,
    )

    private fun write(f: File, text: String, mode: Int = 420) {
        f.parentFile.mkdirs()
        f.writeText(text)
        NioFsOps.chmod(f.path, mode)
    }

    /** A small install dir exercising every rule. */
    private fun sourceTree(): File {
        val d = tmp.newFolder("src")
        write(File(d, "rootfs/etc/hostname"), "box\n")
        write(File(d, "rootfs/usr/bin/tool"), "#!/bin/sh\n", 2541) // 04755
        write(File(d, "rootfs/root/secret"), "s", 0)               // unreadable
        Files.createSymbolicLink(File(d, "rootfs/lib").toPath(), File("usr/lib").toPath())
        File(d, "rootfs/usr/lib").mkdirs()
        Files.createSymbolicLink(File(d, "rootfs/usr/lib/libx.so").toPath(), File("tawcroot:link:42").toPath())
        File(d, "rootfs/locked").mkdirs()
        write(File(d, "rootfs/locked/inner"), "in")
        NioFsOps.chmod(File(d, "rootfs/locked").path, 320) // 0500
        write(File(d, "rootfs/tmp/stale.sock"), "")
        NioFsOps.chmod(File(d, "rootfs/tmp").path, 1023) // 01777
        write(File(d, "tawcroot/version"), "1\n")
        write(File(d, "tawcroot/link/42"), "shared\n")
        write(File(d, "tawcroot/link/42.cnt"), "0000000002\n")
        write(File(d, "tawcroot/lock"), "")
        write(File(d, "tawcroot/intent.new"), "torn")
        write(File(d, "tawcroot/work/42.src"), "parked")
        write(File(d, "tawcroot/tmp/123"), "orphan")
        write(File(d, "ando/ando.sock"), "")
        write(File(d, "metadata.json"), meta.toJson())
        File(d, "rootfs/etc/hostname").setLastModified(1_600_000_000_000L)
        NioFsOps.setXattr(File(d, "rootfs/etc/hostname").path, "user.xdg.origin.url", "https://x/".toByteArray())
        return d
    }

    private fun export(src: File): ByteArray {
        val out = ByteArrayOutputStream()
        DistroExporter.ArchiveWriter(src, NioFsOps) {}.write(
            manifest(), File(src, "metadata.json").readBytes(), out,
        ) { _, _ -> }
        return out.toByteArray()
    }

    private fun reader(bytes: ByteArray) = DistroImporter.Reader(ByteArrayInputStream(bytes)) { it }

    private fun import(bytes: ByteArray, dest: File): DistroImporter.Header {
        val r = reader(bytes)
        val h = r.readHeader()
        r.extractTo(dest, {}, {}, NioFsOps)
        return h
    }

    @Test
    fun roundTripKeepsContentModesLinksAndMtimes() {
        val src = sourceTree()
        val bytes = export(src)
        val dest = File(tmp.root, "dest")
        val h = import(bytes, dest)

        assertEquals("src", h.manifest.id)
        assertEquals(meta, h.metadata)
        assertEquals("box\n", File(dest, "rootfs/etc/hostname").readText())
        assertEquals(1_600_000_000_000L, File(dest, "rootfs/etc/hostname").lastModified())
        val xa = NioFsOps.userXattrs(File(dest, "rootfs/etc/hostname").path)
        assertEquals(listOf("user.xdg.origin.url"), xa.map { it.first })
        assertEquals("https://x/", String(xa.single().second))
        assertEquals(2541, NioFsOps.lstat(File(dest, "rootfs/usr/bin/tool").path).perm)
        assertEquals(0, NioFsOps.lstat(File(dest, "rootfs/root/secret").path).perm)
        assertEquals(0, NioFsOps.lstat(File(src, "rootfs/root/secret").path).perm) // restored
        assertEquals("usr/lib", NioFsOps.readlink(File(dest, "rootfs/lib").path))
        assertEquals("tawcroot:link:42", NioFsOps.readlink(File(dest, "rootfs/usr/lib/libx.so").path))
        assertEquals(320, NioFsOps.lstat(File(dest, "rootfs/locked").path).perm)
        assertEquals(1023, NioFsOps.lstat(File(dest, "rootfs/tmp").path).perm)
        assertEquals("shared\n", File(dest, "tawcroot/link/42").readText())
        assertEquals("0000000002\n", File(dest, "tawcroot/link/42.cnt").readText())
    }

    @Test
    fun exclusionsAreHonored() {
        val dest = File(tmp.root, "dest")
        import(export(sourceTree()), dest)
        assertTrue(File(dest, "rootfs/tmp").isDirectory)
        assertFalse(File(dest, "rootfs/tmp/stale.sock").exists())
        assertFalse(File(dest, "tawcroot/lock").exists())
        assertFalse(File(dest, "tawcroot/intent.new").exists())
        assertTrue(File(dest, "tawcroot/work").isDirectory)
        assertFalse(File(dest, "tawcroot/work/42.src").exists())
        assertFalse(File(dest, "tawcroot/tmp/123").exists())
        assertFalse(File(dest, "ando").exists())
        // metadata.json is the importer's to write, never extracted.
        assertFalse(File(dest, "metadata.json").exists())
    }

    @Test
    fun pendingIntentFailsTheExport() {
        val src = sourceTree()
        write(File(src, "tawcroot/intent"), "journal")
        try {
            export(src)
            fail("export with a pending intent must fail")
        } catch (e: IOException) {
            assertTrue(e.message!!.contains("intent"))
        }
    }

    @Test
    fun truncatedArchiveFails() {
        val bytes = export(sourceTree())
        // Cut just before the trailer entry: everything that's left is
        // well-formed tar, so only the missing end marker can catch it.
        val cut = indexOf(bytes, DistroArchive.TRAILER.toByteArray()) - 1
        assertImportFails(bytes.copyOf(cut - cut % 512), "truncated")
        assertImportFails(bytes.copyOf(bytes.size / 2), "")
    }

    @Test
    fun tamperedContentFailsTheTrailerCheck() {
        val bytes = export(sourceTree())
        val i = indexOf(bytes, "box\n".toByteArray())
        bytes[i] = 'f'.code.toByte()
        assertImportFails(bytes, "corrupt")
    }

    @Test
    fun tooNewFormatIsRefused() {
        val bytes = hostile(manifestJson = manifest().toJson().replace("\"format\": ${DistroArchive.FORMAT}", "\"format\": 99"))
        try {
            reader(bytes).readHeader()
            fail()
        } catch (e: IOException) {
            assertTrue(e.message, e.message!!.contains("newer"))
        }
    }

    @Test
    fun notAnExportIsRefused() {
        val out = ByteArrayOutputStream()
        TarArchiveOutputStream(out).use { t -> file(t, "etc/passwd", "root") }
        try {
            reader(out.toByteArray()).readHeader()
            fail()
        } catch (e: IOException) {
            assertTrue(e.message!!.contains("not a TAWC distro export"))
        }
    }

    @Test
    fun hostileEntriesAreRejected() {
        val outside = File(tmp.root, "outside").apply { mkdirs() }
        val cases: List<(TarArchiveOutputStream) -> Unit> = listOf(
            { t -> file(t, "../escape", "x") },
            { t ->
                t.putArchiveEntry(TarArchiveEntry("/rootfs/abs", true).apply { size = 1 })
                t.write(byteArrayOf(1))
                t.closeArchiveEntry()
            },
            { t -> file(t, "rootfs/../metadata.json", "{}") },
            { t -> file(t, "ando/ando.sock", "x") },
            { t -> file(t, "metadata.json", "{}") },
            { t -> file(t, "tawcroot/intent", "x") },
            { t -> file(t, "tawcroot/work/1.add", "x") },
            { t -> symlink(t, "rootfs", outside.path) },
            { t -> symlink(t, "tawcroot/link", outside.path) },
            { t -> symlink(t, "launcher", outside.path) },
            { t -> dir(t, "launcher/"); dir(t, "launcher/icons/"); symlink(t, "launcher/icons/a.png", "../../rootfs/etc/shadow") },
            { t ->
                dir(t, "launcher/")
                t.putArchiveEntry(TarArchiveEntry("launcher/h", TarConstants.LF_LINK).apply { linkName = "rootfs/x" })
                t.closeArchiveEntry()
            },
            // symlink-then-write-through, out of the slot and onto a sibling
            { t -> dir(t, "rootfs/"); symlink(t, "rootfs/evil", outside.path); file(t, "rootfs/evil/x", "x") },
            { t -> dir(t, "rootfs/"); symlink(t, "rootfs/m", "../metadata.json"); file(t, "rootfs/m", "x") },
            { t ->
                dir(t, "rootfs/")
                t.putArchiveEntry(TarArchiveEntry("rootfs/h", TarConstants.LF_LINK).apply { linkName = "../x" })
                t.closeArchiveEntry()
            },
        )
        for ((i, case) in cases.withIndex()) {
            // The importer writes the slot's metadata before extracting.
            val dest = File(tmp.root, "dest$i").apply { mkdirs() }
            File(dest, "metadata.json").writeText("orig")
            try {
                val r = reader(hostile(entries = case))
                r.readHeader()
                r.extractTo(dest, {}, {}, NioFsOps)
                fail("case $i was accepted")
            } catch (_: IOException) {
            }
            assertFalse("case $i wrote outside", File(outside, "x").exists())
            assertEquals("case $i wrote metadata", "orig", File(dest, "metadata.json").readText())
        }
    }

    @Test
    fun rewriteCarriesSettingsAndForcesRefresh() {
        val r = DistroImporter.rewrite(meta, "new", "New", "me.phie.tawc", 7L, allFilesDeclared = true)
        assertEquals("new", r.id)
        assertEquals("New", r.label)
        assertEquals(Installation.State.INSTALLING, r.state)
        assertNull(r.tawcStamp)
        assertEquals(meta.tawcInstalls, r.tawcInstalls)
        assertEquals(meta.externalBinds, r.externalBinds)
        assertEquals(meta.legacyHiddenDesktopIds, r.legacyHiddenDesktopIds)
        assertTrue(r.andoEnabled)
        assertEquals(meta.installedAtAppVersionCode, r.installedAtAppVersionCode)
        assertEquals(7L, r.importedAtMillis)
        assertEquals("me.phie.tawc", r.importedFromPackage)
        assertEquals(r, Installation.fromJson(r.toJson()))

        val noShared = DistroImporter.rewrite(meta, "new", null, "", 7L, allFilesDeclared = false)
        assertEquals(listOf(ExternalBind("/", "/android", readOnly = true)), noShared.externalBinds)
        assertNull(noShared.importedFromPackage)
    }

    @Test
    fun incompatibilityChecks() {
        val h = DistroImporter.Header(manifest(), meta)
        assertNull(DistroImporter.incompatibility(h, "arm64-v8a"))
        assertNotNull(DistroImporter.incompatibility(h, "x86_64"))
        assertNotNull(DistroImporter.incompatibility(h.copy(metadata = meta.copy(method = "proot")), "arm64-v8a"))
        // A distro this build doesn't know (newer app, custom) passes on arch alone.
        assertNull(DistroImporter.incompatibility(h.copy(metadata = meta.copy(distro = "fedora")), "arm64-v8a"))
        assertNull(DistroImporter.incompatibility(h.copy(metadata = meta.copy(distro = "custom")), "arm64-v8a"))
    }

    @Test
    fun manifestAndTrailerJsonRoundTrip() {
        val m = manifest().copy(uncompressedBytes = 123, entries = 9)
        assertEquals(m, DistroArchive.Manifest.parse(m.toJson()))
        val t = DistroArchive.Trailer(9, "ab")
        assertEquals(t, DistroArchive.Trailer.parse(t.toJson()))
        assertEquals("arch-20260102.tawc.tar.zst", DistroArchive.suggestedFileName("arch", 1767355200000L))
    }

    @Test
    fun exportRuleTable() {
        assertEquals(DistroArchive.ExportRule.INCLUDE, DistroArchive.exportRule("rootfs/etc"))
        assertEquals(DistroArchive.ExportRule.DIR_ONLY, DistroArchive.exportRule("rootfs/tmp"))
        assertEquals(DistroArchive.ExportRule.INCLUDE, DistroArchive.exportRule("rootfs/var/tmp"))
        assertEquals(DistroArchive.ExportRule.EXCLUDE, DistroArchive.exportRule("ando"))
        assertEquals(DistroArchive.ExportRule.EXCLUDE, DistroArchive.exportRule("bootstrap-work"))
        assertEquals(DistroArchive.ExportRule.EXCLUDE, DistroArchive.exportRule("metadata.json.tmp"))
        assertEquals(DistroArchive.ExportRule.FAIL, DistroArchive.exportRule("tawcroot/intent"))
        assertEquals(DistroArchive.ExportRule.INCLUDE, DistroArchive.exportRule("launcher/entries.json"))
        assertEquals(DistroArchive.ExportRule.INCLUDE, DistroArchive.exportRule("launcher/icons/a.png"))
        assertEquals(DistroArchive.ExportRule.EXCLUDE, DistroArchive.exportRule("launcher/entries.json.tmp"))
        assertEquals(DistroArchive.ExportRule.EXCLUDE, DistroArchive.exportRule("icon-cache"))
        assertEquals(FsStat.S_IFDIR, FsStat(0x41ED, 0, 0, 1, 0, 0).type)
    }

    // ---- helpers ----------------------------------------------------

    /** A well-formed header followed by [entries] and a valid trailer
     *  over everything, so only the entry checks can reject it. */
    private fun hostile(
        manifestJson: String = manifest().toJson(),
        entries: (TarArchiveOutputStream) -> Unit = {},
    ): ByteArray {
        val out = ByteArrayOutputStream()
        val digest = DistroArchive.EntryDigest()
        val t = object : TarArchiveOutputStream(out) {
            var digesting = true

            override fun putArchiveEntry(entry: TarArchiveEntry) {
                if (digesting) digest.header(entry)
                super.putArchiveEntry(entry)
            }

            override fun write(b: ByteArray, off: Int, len: Int) {
                if (digesting) digest.content(b, off, len)
                super.write(b, off, len)
            }
        }
        t.setLongFileMode(TarArchiveOutputStream.LONGFILE_POSIX)
        file(t, DistroArchive.MANIFEST, manifestJson)
        file(t, DistroArchive.METADATA, meta.toJson())
        entries(t)
        val trailer = DistroArchive.Trailer(digest.count, digest.hex()).toJson()
        t.digesting = false
        file(t, DistroArchive.TRAILER, trailer)
        t.close()
        return out.toByteArray()
    }

    private fun file(t: TarArchiveOutputStream, name: String, text: String) {
        val b = text.toByteArray()
        t.putArchiveEntry(TarArchiveEntry(name).apply { size = b.size.toLong() })
        t.write(b)
        t.closeArchiveEntry()
    }

    private fun dir(t: TarArchiveOutputStream, name: String) {
        t.putArchiveEntry(TarArchiveEntry(name))
        t.closeArchiveEntry()
    }

    private fun symlink(t: TarArchiveOutputStream, name: String, target: String) {
        t.putArchiveEntry(TarArchiveEntry(name, TarConstants.LF_SYMLINK).apply { linkName = target })
        t.closeArchiveEntry()
    }

    private fun assertImportFails(bytes: ByteArray, want: String) {
        try {
            import(bytes, File(tmp.root, "dest-fail-${bytes.size}"))
            fail("import of a bad archive succeeded")
        } catch (e: IOException) {
            assertTrue("${e.message} lacks '$want'", e.message!!.contains(want))
        }
    }

    private fun indexOf(hay: ByteArray, needle: ByteArray): Int {
        outer@ for (i in 0..hay.size - needle.size) {
            for (j in needle.indices) if (hay[i + j] != needle[j]) continue@outer
            return i
        }
        error("not found")
    }
}
