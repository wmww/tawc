package me.phie.tawc.install.distro

import me.phie.tawc.install.Installation

/**
 * `MirrorRegion.id` of the China preset. Ids are frozen
 * ([MirrorRegion] "never rename"), so comparing against this is safe;
 * it is also what triggers the CJK-font reminder in the settings row.
 */
internal const val CHINA_MIRROR_REGION_ID = "cn"

/**
 * Command that installs a CJK font in [distro], or `null` when its
 * package manager is unknown to us.
 *
 * Shown as a reminder after the user switches to the China preset: the
 * mirror only changes where packages come from, and none of these
 * minimal rootfs images ships a CJK font, so Chinese text renders as
 * boxes until one is installed.
 *
 * Deliberately not a `Distro` member: nothing in the install path reads
 * it, and the package name is a packaging detail that can change
 * without touching the distro's policy hooks. `xbps-install -S` because
 * a fresh Void rootfs has a stale index exactly once.
 *
 * Ubuntu has no entry here yet (its `DISTRO_UBUNTU` key lands in a
 * separate PR): that distro falls through to the generic hint.
 */
internal fun cjkFontCommand(distro: Distro): String? = when (distro.key) {
    Installation.DISTRO_ARCH,
    Installation.DISTRO_MANJARO,
    -> "pacman -S noto-fonts-cjk"

    Installation.DISTRO_DEBIAN_SID -> "apt-get install -y fonts-noto-cjk"
    Installation.DISTRO_VOID -> "xbps-install -S noto-fonts-cjk"
    else -> null
}
