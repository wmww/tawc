# TawcInstaller per-rootfs refresh failures never surface to the user

`TawcInstaller.installAll` (`TawcInstaller.kt:169-179`, run from
`TawcApplication.onCreate` on every app start) catches per-rootfs
exceptions, logs them with `Log.e`, and moves on. That's the right
call for not blocking app launch, and the mismatched `tawcStamp`
means it retries on the next start — but if a rootfs fails to refresh
*persistently* (permissions, disk full, half-deleted tree), the user
gets no indication at all. The rootfs keeps running with stale
tawc-shipped files (old libhybris/mesa-zink/gfxstream/ando/bashrc)
that silently diverge from the installed app version; the only
evidence is logcat.

Possible fix: record the failure (e.g. on the Installation record or
an in-memory registry) and surface it in the distro list / distro
info screen ("tawc files failed to update — <error>"), so persistent
divergence is visible without adb.
