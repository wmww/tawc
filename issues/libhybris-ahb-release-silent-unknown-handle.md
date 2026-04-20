# libhybris AHB gralloc: release on unknown handle silently returns 0

## Summary

`hybris_gralloc_release` in the AHB backend (`libhybris/hybris/gralloc/gralloc.c`)
calls `ahb_map_decref_take(handle)` and takes the `else` branch on any non-
removal outcome:

```c
AHardwareBuffer *ahb = ahb_map_decref_take(handle);
if (ahb) {
    ahb_release_sym(ahb);
    ret = 0;
} else {
    // refcount still > 0, nothing to do
    ret = 0;
}
```

`ahb_map_decref_take` also returns `NULL` when the handle isn't in the map at
all (e.g. double-release, or release of a handle that was never allocated
through our AHB path). The release path treats "refcount went from 3 to 2" and
"this handle is not tracked" identically, reporting success.

## Impact

Masks real bugs. A caller that double-releases, or uses a handle that escaped
our refcount bookkeeping, gets a success return and no indication that
anything is wrong. Symptoms would show up later as a use-after-free on the
AHB pointer somewhere else entirely, making the root cause hard to trace.

Low likelihood of hitting in practice (we control the call sites), but the
masking property undermines future debugging.

## Fix

Differentiate "not found" from "refcount still positive" in
`ahb_map_decref_take` — e.g. a tri-state return (`found-and-released`,
`found-still-live`, `not-found`) — and return `-EINVAL` (or log a warning) in
the `not-found` case from `hybris_gralloc_release`.
