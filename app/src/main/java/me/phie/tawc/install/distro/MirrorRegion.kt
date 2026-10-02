package me.phie.tawc.install.distro

/**
 * One user-selectable package-mirror location (settings → distro card).
 *
 * [servers] are mirror base URLs in priority order. Pacman walks a
 * mirrorlist top to bottom and falls through to the next entry when one
 * 404s, so a preset lists several hosts rather than a single "best"
 * one — a stale regional mirror otherwise aborts a whole transaction
 * mid-sync. The repository path after the base differs per repo layout
 * (`$arch/$repo` for ALARM, `$repo/os/$arch` for Arch x86_64,
 * `arm-testing/$repo/$arch` for Manjaro ARM, `/current/<arch>` for
 * Void), so the distro owns that suffix and this type only carries the
 * bases. A distro whose config format takes a single URL (apt's
 * `URIs:`) uses the first entry and ignores the rest.
 *
 * [id] is persisted in `metadata.json` (`Installation.mirrorRegion`), so
 * it is a frozen value: relabel freely, never rename. `null` there means
 * the distro's built-in default list — the same behaviour as before this
 * setting existed.
 */
data class MirrorRegion(
    val id: String,
    val label: String,
    val servers: List<String>,
)

/**
 * Built-in mirror presets, grouped by repository layout and architecture
 * (a host that carries ALARM rarely carries Arch x86_64 or Void, and
 * some carry only one architecture).
 *
 * Only hosts verified to still serve the matching tree are listed —
 * checked with a `core.db` / `core/os/x86_64/core.db` /
 * `aarch64-repodata` / `binary-arm64/Release` fetch at the time of
 * writing. Keep that rule when adding entries: a dead first entry costs
 * every user a round trip on every transaction. Note that ALARM's
 * regional hosts are plain `http://`: only `fl.us` and `ca.us` carry a
 * certificate valid for their own hostname, and pacman verifies package
 * signatures independently of transport (see `ArchLinuxArm`'s default
 * list).
 */
internal object MirrorRegions {

    /** Presets for Arch Linux ARM (`aarch64`). */
    val archLinuxArm: List<MirrorRegion> = listOf(
        MirrorRegion(
            "cn",
            "China",
            listOf(
                "https://mirrors.tuna.tsinghua.edu.cn/archlinuxarm",
                "https://mirrors.ustc.edu.cn/archlinuxarm",
                "https://mirror.sjtu.edu.cn/archlinuxarm",
                "https://mirrors.nju.edu.cn/archlinuxarm",
                "https://mirrors.bfsu.edu.cn/archlinuxarm",
                "https://mirror.iscas.ac.cn/archlinuxarm",
                "https://mirrors.cernet.edu.cn/archlinuxarm",
            ),
        ),
        MirrorRegion("de", "Germany", listOf(
            "http://de.mirror.archlinuxarm.org",
            "http://de3.mirror.archlinuxarm.org",
        )),
        MirrorRegion("dk", "Denmark", listOf("http://dk.mirror.archlinuxarm.org")),
        MirrorRegion("fr", "France", listOf("http://fr.mirror.archlinuxarm.org")),
        MirrorRegion(
            "us",
            "United States",
            listOf(
                "https://fl.us.mirror.archlinuxarm.org",
                "https://ca.us.mirror.archlinuxarm.org",
                "http://nj.us.mirror.archlinuxarm.org",
            ),
        ),
        MirrorRegion(
            "world",
            "Worldwide (geo redirector)",
            listOf("http://mirror.archlinuxarm.org"),
        ),
    )

    /** Presets for Manjaro ARM (aarch64). All paths include `arm-testing`. */
    val manjaroArm: List<MirrorRegion> = listOf(
        MirrorRegion("at", "Austria", listOf("https://mirror.alwyzon.net/manjaro/arm-testing")),
        MirrorRegion("be", "Belgium", listOf("https://mirror.futureweb.be/manjaro/arm-testing")),
        MirrorRegion("dk", "Denmark", listOf("https://mirrors.dotsrc.org/manjaro/arm-testing")),
        MirrorRegion("tr", "Türkiye", listOf("https://ftp.linux.org.tr/manjaro/arm-testing")),
    )

    /**
     * Presets for Debian (sid). Entries are archive roots, i.e. the
     * `URIs:` value; apt takes one per region (see [MirrorRegion]).
     */
    val debian: List<MirrorRegion> = listOf(
        MirrorRegion("ch", "Switzerland", listOf("https://mirror.init7.net/debian")),
        MirrorRegion("cn", "China", listOf("https://mirrors.tuna.tsinghua.edu.cn/debian")),
        MirrorRegion("nl", "Netherlands", listOf("https://mirror.leaseweb.com/debian")),
        MirrorRegion("us", "United States", listOf("http://ftp.us.debian.org/debian")),
        MirrorRegion("world", "Worldwide", listOf("https://ftp.debian.org/debian")),
    )

    /**
     * Presets for Ubuntu on arm64, which lives on the `ports` archive
     * (`.../ubuntu-ports`); amd64 would use `archive.ubuntu.com/ubuntu`
     * bases instead. Entries are archive roots, i.e. the `URIs:` value.
     *
     * Ubuntu is not a shipped distro yet — `distro/ubuntu/` is still a
     * plan (plans/ubuntu-distro.md) — so nothing consumes this table
     * today. It is here because the region data is the part that has to
     * be verified against upstream by hand, and it belongs next to the
     * other families rather than after them.
     */
    val ubuntu: List<MirrorRegion> = listOf(
        MirrorRegion(
            "cn",
            "China",
            listOf(
                "https://mirrors.tuna.tsinghua.edu.cn/ubuntu-ports",
                "https://mirrors.ustc.edu.cn/ubuntu-ports",
                "https://mirror.sjtu.edu.cn/ubuntu-ports",
                "https://mirrors.nju.edu.cn/ubuntu-ports",
                "https://mirrors.bfsu.edu.cn/ubuntu-ports",
                "https://mirrors.cernet.edu.cn/ubuntu-ports",
            ),
        ),
        MirrorRegion("nl", "Netherlands", listOf("https://mirror.leaseweb.com/ubuntu-ports")),
        MirrorRegion("world", "Worldwide", listOf("https://ports.ubuntu.com/ubuntu-ports")),
    )

    /**
     * Presets for Void Linux. Entries are mirror roots *without*
     * `/current`: the distro appends the release path and, on ports, the
     * architecture subdirectory.
     */
    val voidLinux: List<MirrorRegion> = listOf(
        MirrorRegion(
            "cn",
            "China",
            listOf(
                "https://mirrors.tuna.tsinghua.edu.cn/voidlinux",
                "https://mirror.sjtu.edu.cn/voidlinux",
                "https://mirrors.nju.edu.cn/voidlinux",
                "https://mirrors.bfsu.edu.cn/voidlinux",
                "https://mirrors.cernet.edu.cn/voidlinux",
            ),
        ),
        MirrorRegion("world", "Worldwide", listOf("https://repo-default.voidlinux.org")),
    )

    /** Presets for Arch Linux x86_64 (the emulator ABI). */
    val archLinux: List<MirrorRegion> = listOf(
        MirrorRegion("ch", "Switzerland", listOf("https://mirror.init7.net/archlinux")),
        MirrorRegion(
            "cn",
            "China",
            listOf(
                "https://mirrors.tuna.tsinghua.edu.cn/archlinux",
                "https://mirrors.ustc.edu.cn/archlinux",
                "https://mirror.sjtu.edu.cn/archlinux",
                "https://mirrors.nju.edu.cn/archlinux",
                "https://mirrors.bfsu.edu.cn/archlinux",
                "https://mirrors.cernet.edu.cn/archlinux",
                "https://mirrors.aliyun.com/archlinux",
            ),
        ),
        MirrorRegion("de", "Germany", listOf("https://ftp.halifax.rwth-aachen.de/archlinux")),
        MirrorRegion("nl", "Netherlands", listOf("https://mirror.leaseweb.com/archlinux")),
        MirrorRegion(
            "us",
            "United States",
            listOf(
                "https://mirror.rackspace.com/archlinux",
                "https://mirrors.kernel.org/archlinux",
            ),
        ),
        MirrorRegion(
            "world",
            "Worldwide (geo redirector)",
            listOf("https://geo.mirror.pkgbuild.com"),
        ),
    )
}
