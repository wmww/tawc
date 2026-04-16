# `MakeCurrent` return value ignored in render path

## Summary

`server/compositor/src/render.rs::ensure_wlegl_texture` calls
`smithay::backend::egl::ffi::egl::MakeCurrent` directly and discards the
return value. If the call fails (e.g. the context was lost — Adreno
drivers do drop contexts on resume from sleep), the subsequent
`importer.import_ahb` runs against whatever context is current,
producing a texture that isn't usable from our renderer's context, or
GL errors that we then silently turn into "import failed".

The same pattern existed in the deleted side-channel `import_one_ahb`.

## Where

`server/compositor/src/render.rs:215` (approx — inside
`ensure_wlegl_texture`).

## Fix

Check the return value and return `false` with an error log on failure.
Better: route the import through Smithay's `EGLContext::make_current`
so context loss is detected and surfaced via `EglError`. The custom
`unsafe` MakeCurrent path predates the C helper rewrite — it was
needed when we had to switch contexts, but now that the import only
uses the renderer's own context we can use Smithay's safe wrapper.

## Impact

Low day-to-day (Pixel 4a / OnePlus 9 don't drop contexts in practice),
but a confusing failure mode if it ever does happen — the only signal
is `eglCreateImageKHR failed, EGL error: 0x...` with no hint that the
context was the problem.
