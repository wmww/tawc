# buildLibhybris / packLibhybris Gradle tasks lack inputs/outputs declarations

The two `Exec` tasks added by the libhybris-as-asset commit (`8b198ed`)
in `server/app/build.gradle.kts` — `buildLibhybris` and `packLibhybris`
— declare neither `inputs.files(...)` nor `outputs.dir/file(...)`. Both
hang off `preBuild` via `dependsOn`.

Without input/output declarations Gradle has no way to compute an
up-to-date check on either task, with two flavours of broken behaviour
depending on cache state:

- **The slow case (today's default).** Every `assembleDebug` re-runs
  the libhybris cross-compile from scratch — several minutes — even
  when nothing under `./libhybris/` has changed.
- **The wrong case.** If Gradle's daemon ever caches a task as
  up-to-date (e.g. via configuration-cache replay), changes *inside*
  `./libhybris/hybris/` get silently ignored and we ship a stale
  tarball. There's no signal that this happened.

## The fix

Teach each task what it consumes and produces:

```kotlin
val buildLibhybrisTask = tasks.register<Exec>("buildLibhybris") {
    workingDir = tawcRoot
    commandLine("bash", "client/build-libhybris-aarch64")
    inputs.files(fileTree("$tawcRoot/libhybris/hybris"))
    inputs.file("$tawcRoot/client/build-libhybris-aarch64")
    outputs.dir("$libhybrisInstallDir/lib")
}

val packLibhybrisTask = tasks.register<Exec>("packLibhybris") {
    dependsOn(buildLibhybrisTask)
    doFirst { mkdir(file(libhybrisAssetFile).parentFile) }
    workingDir = file(libhybrisInstallDir)
    commandLine("tar", "--format=ustar",
        "--exclude=*.la", "--exclude=pkgconfig",
        "-cf", "${project.projectDir}/$libhybrisAssetFile", "lib")
    inputs.dir("$libhybrisInstallDir/lib")
    outputs.file(libhybrisAssetFile)
}
```

The exact `inputs.files(...)` set for `buildLibhybris` depends on what
`client/build-libhybris-aarch64` actually reads — that script needs an
audit before declaring inputs, since under-declaring causes stale-
tarball bugs (worse than today's slow behaviour) and over-declaring
just costs incremental builds. Likely candidates:
`./libhybris/hybris/**` (the source tree), the build script itself,
`./android-headers/**` (kernel header overrides — though this is its
own dep tree; maybe pin a version stamp instead), the NDK toolchain
location.

## Tangentially related

`buildLibhybris` also doesn't declare a dependency on the
`./android-headers` clone existing. If the user has a fresh checkout
without it, the task fails with a `git clone` hint message — fine as a
human-facing error, but it'd be nicer to wrap that in a Gradle task
that conditionally runs the clone. Out of scope for this issue.

## Why we didn't fix it inline

Spotted during a code review of unrelated proot changes. Pre-existing
in `8b198ed` (libhybris-as-asset), not introduced by the proot work.
Worth landing on its own so the input-set audit can happen carefully.
