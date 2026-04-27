package me.phie.tawc.install.distro

import me.phie.tawc.install.Installation
import me.phie.tawc.install.distro.arch.ArchLinuxArm
import me.phie.tawc.install.distro.arch.ArchLinuxX86_64
import me.phie.tawc.install.util.HostArch

/**
 * Catalogue of supported [Distro] implementations and the only place
 * that maps `(metadata.distro, metadata.arch)` to a concrete instance.
 *
 * Existing on-disk records use `distro = "arch"` for both Arch Linux
 * and Arch Linux ARM, so disambiguation is by Android ABI.
 */
object DistroRegistry {
    val all: List<Distro> = listOf(
        ArchLinuxX86_64,
        ArchLinuxArm,
    )

    /**
     * Resolve [inst]'s metadata back to the implementation. Returns
     * null if the (distro, arch) pair has no match — the caller should
     * surface this as a refused operation rather than silently
     * succeeding.
     */
    fun forInstallation(inst: Installation): Distro? =
        all.firstOrNull { it.key == inst.distro && it.androidAbi == inst.arch }

    /**
     * Distro auto-selected for a fresh install on this host. Today
     * this is a 1:1 ABI→Distro lookup; once we support multiple distro
     * families (Ubuntu, Fedora, …) the install activity will grow a
     * picker that filters [all] by ABI and the service will accept a
     * `--es distro <key>` extra.
     */
    fun defaultForHost(): Distro? =
        all.firstOrNull { it.androidAbi == HostArch.primaryAbi() }
}
