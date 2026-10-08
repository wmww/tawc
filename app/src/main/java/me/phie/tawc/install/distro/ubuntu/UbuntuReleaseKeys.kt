package me.phie.tawc.install.distro.ubuntu

/**
 * Ubuntu's cdimage signing key, the root of trust for an Ubuntu install.
 *
 * Ubuntu signs `SHA256SUMS` at
 * `cdimage.ubuntu.com/ubuntu-base/releases/24.04/release/` with the
 * *Ubuntu CD Image Automatic Signing Key (2012)*, and
 * [UbuntuSha256Resolver] checks that signature before it trusts any
 * digest out of the manifest. The manifest and the tarball share one
 * origin, so the digest alone would only be a corruption check; the
 * signature is what makes a mirror (or cdimage itself) unable to hand us
 * a different rootfs.
 *
 * Deliberately a Kotlin constant rather than a `res/raw` key like the
 * Arch ones: `Distro.resolveBootstrap` runs before any `Context` is in
 * play, and this verification happens at resolve time. Same reason
 * [me.phie.tawc.install.distro.voidlinux.VoidReleaseKeys] bundles its
 * keys in code. The Arch keys stay in `res/raw` because their check runs
 * later, inside [me.phie.tawc.install.SignatureVerifier.verify], which
 * does have a `Context`.
 *
 * The armored text below is a verbatim `gpg --armor --export` of
 * [FINGERPRINT], fetched 2026-10-01 and cross-checked against three
 * origins before shipping:
 *
 *  1. keyserver.ubuntu.com;
 *  2. keys.openpgp.org (same fingerprint, same creation time
 *     2012-05-11, RSA-4096);
 *  3. `etc/apt/trusted.gpg.d/ubuntu-keyring-2012-cdimage.gpg` inside
 *     `ubuntu-base-24.04.5-base-arm64.tar.gz` — whose SHA-256 is itself
 *     covered by the manifest this key verifies. `gpg --export` of that
 *     copy and of the keyserver copy are byte-identical.
 *
 * Re-run at least two of those before changing anything here.
 */
internal object UbuntuReleaseKeys {

    /**
     * `Ubuntu CD Image Automatic Signing Key (2012) <cdimage@ubuntu.com>`,
     * rsa4096, created 2012-05-11, no published expiry.
     *
     * Not to be confused with the cloud-image key
     * (`UEC Image Automatic Signing Key`), which signs cloud images, not
     * `ubuntu-base`.
     */
    const val FINGERPRINT: String = "843938DF228D22F7B3742BC0D94AA3F0EFE21092"

    /** ASCII-armored public key, verbatim from upstream. */
    const val CDIMAGE_SIGNING_KEY: String = """-----BEGIN PGP PUBLIC KEY BLOCK-----

mQINBE+tjmgBEAC7pKK78t89DW7mvMoSgiScLfPNF8/TSF380is0hFRL3dOmcXEf
NsX26jtv8bdvvtkElB1fPwOntmqSAsrLOuURVQ6GSxH7IDU5QFfaTIsudtLR5YTl
C3ZuOTOb1HWEK26fDRXuIWjhFDXJH3KLv+rSrq0+x7ZtH++CHq5XJWk7VUh/wWcG
xZefs7+1HTivymhjXCOwQvqblzZ5MAec9i4QIXxkqX1HY7ryxGVdjj9lApOnoU5E
cSYr08cm7xQEgrdDLAZFQxDYBLDuV6E6jKEfAfwZINSEe4Ocm82vtCF5K0HiwhFU
09ky2yogbMuTTi2f8ibN8SbbhZDJlDPd2ZkkpsKNfIALmOiPhHGvXGmtg6FdzRUO
SGirSm8tcakpS+d0/IElbD453sksxg6s3cTs7Q+PudaccyQ0BqatMnzmfxCVOotT
65kVnmz2P+4Q0gRSQ/Zi9Inz+OrzWxtn6/Tdw+FMUwvBccxW1r88k6uVLz23jW/8
jOuwnUp4JKmZta/U2UZKTyPyrvTYhp/zK332BEnxiRY4ZfQjA4Iwlw00l4pYBDLL
c6TFJtLbDv859UCisXa8MtWYWrlM3YfGFs9k1WemML8u79g2DK8g3VPkD94Q5anq
ufEGm74K/keOmss8cQoBX9VPFMpS1mFCT+2UdGP0UvMlADct0aFnAwtb9QARAQAB
tEFVYnVudHUgQ0QgSW1hZ2UgQXV0b21hdGljIFNpZ25pbmcgS2V5ICgyMDEyKSA8
Y2RpbWFnZUB1YnVudHUuY29tPokCNwQTAQoAIQUCT62OaAIbAwULCQgHAwUVCgkI
CwUWAgMBAAIeAQIXgAAKCRDZSqPw7+IQkkhAEACJjZZXuAabMrC49Z52HywVZipJ
goV5ufMi2LQYMkyGKVQQ/E74lUjccMmbQ4j00ihTYB+F/i29AxfavJnlSpWgmwjP
O4YY5jvooUiXQmVHX10oM1w3+Y9wScmeUY3IhTtwiFaBJr6TZ7RvOTg/pbQ0Gvzx
NlkSobuqFCZ023mcl2Y7OkY1PZgxiLafD6Rx2O/gclQPs4YfHo8bKRA4o10702nE
8YE+dixIgAQw67Txhq5idNxsWpudKq9J1fLgnEz7i9AJUOf12sg9X7ZvpXZ3QvMV
5iOvLA4DRLv9HIxyz70XqeakS+uzfKXuCMzhdUTIb/tNACNB37+reIqdPsyUF3tx
VyWaL1jMkRsv617yKAiYvPNwMDRvrbKiJ4Icnd4tPzmqz5HBFUyULns3JzJNjpgK
CvLGhVq+lVsdpMlpQxEG5/bhzJgB1jrIbkcOSfnQ1y0Gv9CItel+1q0BHMn0dPVW
aNfKYFGsz4igW+uj//C09/gtGMm78PQfjqEoR2j/Tam/tmucxSK331yfm5ag2CQY
GC3bswfII+4EanX9dN/RG3/2dsSyYruWpTIQG6Xa7+AZtYBDEXNYovgdJtXWyUtW
0X7R6vIjh1HYer3dR6ivJ+q/bWGY45zHeNBNU33hlnlxEENif3RZ/j/w3SjGrtSQ
K69maNR6onq492e+6w==
=snmR
-----END PGP PUBLIC KEY BLOCK-----"""

    /** Where [CDIMAGE_SIGNING_KEY] came from, for docs and error messages. */
    const val KEY_ORIGIN: String = "keyserver.ubuntu.com (cross-checked against keys.openpgp.org)"
}
