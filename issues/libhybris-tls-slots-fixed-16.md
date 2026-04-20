# libhybris tls_area: fixed 16-slot array is forward-fragile

## Summary

The bionic-TLS compat struct in `libhybris/hybris/common/hooks.c` is:

```c
static __attribute__((tls_model("initial-exec")))
    __thread struct {
        void *bionic_tls_ptr;   // slot -1 (TLS_SLOT_BIONIC_TLS)
        void *slots[16];        // slots 0..15
    } tls_area;
```

Bionic's well-known TLS slot list has grown over Android releases
(sanitizer state, stack-MTE, shadow-call-stack, etc). A vendor library
compiled against a bionic header that defines a slot >= 16 would index past
`tls_area.slots` and silently corrupt adjacent TLS or heap state.

## Impact

Currently none — every bionic slot we have observed vendor libraries using
fits within 0..15. But the check is compile-time for bionic callers and
runtime-invisible for us, so a regression on a newer OEM GPU stack would
manifest as hard-to-diagnose memory corruption rather than a clean failure.

## Fix options

- Bump `slots` to something generous (e.g. `[64]` or `[128]`) — cheap, per
  thread, just wastes a KB of TLS.
- Add a `_Static_assert` keyed on the highest `TLS_SLOT_*` constant visible
  to us at build time, so a bionic-header update that adds a slot fails to
  compile rather than silently overflowing.

The generous-sizing option is strictly safer and has no downside worth
worrying about.
