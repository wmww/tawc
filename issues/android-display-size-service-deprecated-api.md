# Service-side display sizing still uses deprecated API 29 fallback

`CompositorService.currentDisplaySize()` seeds the Rust compositor with a
non-zero output size before any `CompositorActivity` surface has registered.
For `minSdk=29`, the fallback path uses deprecated
`WindowManager.defaultDisplay` / `Display.getRealSize`.

This is not a user-visible bug today because the first Activity surface
immediately refines the size via `nativeRegisterActivitySurface`, and lint is
clean because the deprecated calls are guarded. It is still awkward Android
API shape: a `Service` is guessing display geometry for UI owned by an
Activity.

Better fix:

- Move initial output sizing to the first `CompositorActivity`/`SurfaceView`
  registration, or add a native "pending output size" update before clients
  can commit buffers.
- Remove `CompositorService.currentDisplaySize()` and the deprecated fallback.
- Re-test early client startup, especially clients that commit immediately
  after the initial configure.

