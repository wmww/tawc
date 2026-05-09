package me.phie.tawc.install.distro.manjaro

import me.phie.tawc.install.MirrorProxy
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

/**
 * Look up the latest GitHub Release for a repo and return the matching
 * asset's download URL plus its server-computed SHA-256.
 *
 * Used by [ManjaroArm] to resolve the `manjaro-arm/rootfs` weekly
 * release at install time. The integrity story rests on the single
 * HTTPS endpoint at api.github.com — the same trust we already place
 * in the GitHub release tarball download itself. The SHA-256 in the
 * `digest` field is computed by GitHub on upload, so a third party
 * tampering with the artifact between upload and our download would
 * fail the post-download check.
 *
 * Trust boundary: anyone with push to the manjaro-arm GitHub org can
 * publish a malicious tarball and a matching digest (same threat as
 * any HTTPS-distributed artifact). PGP would be a stronger story, but
 * Manjaro ARM doesn't sign — see notes/distro-options.md.
 */
internal object GitHubReleaseResolver {

    data class Asset(val downloadUrl: String, val sha256Hex: String)

    /**
     * Fetch the "latest" release for [owner]/[repo] and return the
     * asset whose `name` equals [assetName]. Throws [IOException] if
     * the release lacks the asset, the asset is missing a server-side
     * `digest`, or anything else goes wrong.
     */
    fun resolveLatest(
        owner: String,
        repo: String,
        assetName: String,
        mirrorProxy: MirrorProxy? = null,
    ): Asset {
        val apiUrl = "https://api.github.com/repos/$owner/$repo/releases/latest"
        val body = downloadString(mirrorProxy?.wrap(apiUrl) ?: apiUrl)
        val json = JSONObject(body)
        val assets = json.optJSONArray("assets")
            ?: throw IOException("GitHub release JSON has no `assets` array (url=$apiUrl)")
        for (i in 0 until assets.length()) {
            val a = assets.getJSONObject(i)
            if (a.optString("name") != assetName) continue
            val url = a.optString("browser_download_url")
                .takeIf { it.isNotEmpty() }
                ?: throw IOException(
                    "Asset '$assetName' in $owner/$repo latest release has no browser_download_url",
                )
            // The `digest` field looks like "sha256:abc123…". GitHub
            // sets this server-side at upload time. Bail loudly if
            // it's missing — silently falling back to None on a hash
            // we explicitly opted into would defeat the purpose.
            val rawDigest = a.optString("digest")
                .takeIf { it.isNotEmpty() }
                ?: throw IOException(
                    "Asset '$assetName' in $owner/$repo latest release has no `digest` field — " +
                        "GitHub may have changed its API; refusing to install without integrity check",
                )
            val sha = rawDigest.removePrefix("sha256:").lowercase()
            if (sha.length != 64 || !sha.all { it.isDigit() || it in 'a'..'f' }) {
                throw IOException(
                    "Asset '$assetName' digest '$rawDigest' is not a sha256 hex — " +
                        "expected 'sha256:<64 hex>'",
                )
            }
            return Asset(url, sha)
        }
        throw IOException(
            "Asset '$assetName' not found in $owner/$repo latest release (saw " +
                "${(0 until assets.length()).map { assets.getJSONObject(it).optString("name") }})",
        )
    }

    private fun downloadString(url: String): String {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 30_000
            readTimeout = 30_000
            instanceFollowRedirects = true
            // Pin to a known API version so a future GitHub-side
            // schema change can't surprise us (e.g. removing `digest`).
            setRequestProperty("Accept", "application/vnd.github+json")
            setRequestProperty("X-GitHub-Api-Version", "2022-11-28")
            setRequestProperty("User-Agent", "tawc-installer")
        }
        try {
            val code = conn.responseCode
            if (code !in 200..299) {
                throw IOException("GET $url returned HTTP $code")
            }
            return conn.inputStream.use { String(it.readBytes(), Charsets.UTF_8) }
        } finally {
            conn.disconnect()
        }
    }
}
