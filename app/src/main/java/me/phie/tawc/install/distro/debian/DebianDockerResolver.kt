package me.phie.tawc.install.distro.debian

import me.phie.tawc.install.BootstrapFormat
import me.phie.tawc.install.BootstrapVerification
import me.phie.tawc.install.MirrorProxy
import me.phie.tawc.install.distro.DistroBootstrap
import org.json.JSONObject
import java.io.IOException
import java.net.HttpURLConnection
import java.net.URL

internal object DebianDockerResolver {
    private const val RAW_BASE = "https://raw.githubusercontent.com/debuerreotype/docker-debian-artifacts"

    fun resolve(
        suite: String,
        bashbrewArch: String,
        mirrorProxy: MirrorProxy?,
    ): DistroBootstrap {
        val branch = "dist-$bashbrewArch"
        val base = "$RAW_BASE/$branch/$suite/oci/blobs"
        val manifestUrl = "$base/image-manifest.json"
        val manifest = JSONObject(downloadText(mirrorProxy?.wrap(manifestUrl) ?: manifestUrl))
        val layers = manifest.getJSONArray("layers")
        if (layers.length() != 1) {
            throw IOException("Debian $suite $bashbrewArch manifest has ${layers.length()} layers, expected 1")
        }
        val layer = layers.getJSONObject(0)
        val mediaType = layer.getString("mediaType")
        if (mediaType != "application/vnd.oci.image.layer.v1.tar+gzip") {
            throw IOException("Debian $suite $bashbrewArch layer has unsupported mediaType $mediaType")
        }
        val digest = layer.getString("digest").removePrefix("sha256:")
        return DistroBootstrap(
            url = "$base/rootfs.tar.gz",
            format = BootstrapFormat.GZIP,
            stripPrefix = null,
            verification = BootstrapVerification.Sha256(digest),
        )
    }

    private fun downloadText(url: String): String {
        val conn = (URL(url).openConnection() as HttpURLConnection).apply {
            connectTimeout = 15_000
            readTimeout = 30_000
            instanceFollowRedirects = true
        }
        try {
            val code = conn.responseCode
            if (code !in 200..299) {
                throw IOException("GET $url returned HTTP $code")
            }
            return conn.inputStream.bufferedReader(Charsets.UTF_8).use { it.readText() }
        } finally {
            conn.disconnect()
        }
    }
}
