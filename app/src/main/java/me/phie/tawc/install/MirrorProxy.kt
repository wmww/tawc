package me.phie.tawc.install

import java.net.URL

/**
 * Dev-time URL rewriter that funnels distro-mirror fetches through a
 * host-side caching reverse proxy. See `notes/cache-proxy.md`.
 *
 * Wraps an upstream URL `<scheme>://<host>/<path>?<query>` as
 * `<base>/<scheme>/<host>/<path>?<query>` so a single nginx with the
 * `^/proxy/(https?)/([^/]+)/(.*)$` location block can route to any
 * upstream and key the cache off the upstream URL. The query string
 * follows `proxy_pass` upstream but the cache key strips it (one cache
 * entry per path, regardless of signed-redirect tokens).
 *
 * Verification URLs (PGP `.sig`, ALARM `.md5` sidecars, GitHub
 * Releases REST API) intentionally do **not** flow through this — see
 * notes/cache-proxy.md "What gets cached" for the policy. The
 * security model for cross-mirror cross-checks rests on the two
 * checksum endpoints being independently operated, and proxying both
 * through the same nginx would undo that.
 *
 * Construction is intentionally light — pass the bare base URL the
 * user supplied via `--es mirrorProxy` and we'll cope with a missing
 * trailing slash.
 */
class MirrorProxy(baseUrl: String) {
    /** Always normalised to end with exactly one `/`. */
    val base: String = baseUrl.trimEnd('/') + "/"

    /**
     * Rewrite [url] to traverse this proxy. Throws on non-http(s) URLs;
     * we never need to proxy a `file://` or git ref, so a non-supported
     * scheme is a programming error worth surfacing.
     */
    fun wrap(url: String): String {
        val u = URL(url)
        val scheme = u.protocol
        require(scheme == "http" || scheme == "https") {
            "MirrorProxy.wrap: unsupported scheme '$scheme' in $url"
        }
        // URL.authority is host[:port]; URL.file is path[?query]. Both
        // are pre-encoded forms suitable for direct concatenation.
        val authority = u.authority
        val pathAndQuery = u.file.removePrefix("/")
        return "$base$scheme/$authority/$pathAndQuery"
    }
}
