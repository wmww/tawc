package me.phie.tawc.install.distro

import me.phie.tawc.install.Installation
import me.phie.tawc.install.distro.arch.ArchLinuxArm
import me.phie.tawc.install.distro.arch.ArchLinuxX86_64
import me.phie.tawc.install.distro.manjaro.ManjaroArm
import me.phie.tawc.install.distro.voidlinux.VoidLinuxAarch64
import me.phie.tawc.install.distro.voidlinux.VoidLinuxX86_64
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
        ManjaroArm,
        VoidLinuxX86_64,
        VoidLinuxAarch64,
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
     * Distros that can be installed on this host (matching the host's
     * primary Android ABI). The install activity uses this for its
     * distro radio; the service uses [forKey] to resolve the user's
     * pick.
     */
    fun availableForHost(): List<Distro> =
        all.filter { it.androidAbi == HostArch.primaryAbi() }

    /**
     * Distro auto-selected for a fresh install on this host when the
     * caller doesn't specify one. First match wins — kept for the
     * `am start` autoStart path that doesn't pass a distro extra.
     */
    fun defaultForHost(): Distro? = availableForHost().firstOrNull()

    /**
     * Resolve a `(key, host-abi)` pair to a Distro. Used by
     * [me.phie.tawc.install.InstallationService] to validate the
     * `--es distro` install extra against the device's actual ABI.
     */
    fun forKey(distroKey: String): Distro? =
        availableForHost().firstOrNull { it.key == distroKey }
}
