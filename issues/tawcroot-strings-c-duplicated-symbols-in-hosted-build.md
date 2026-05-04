# tawcroot: hosted (cleat) build links both `strings.c` and glibc's `mem*`

The cleat orchestrator is a hosted glibc build, but `strings.c` (which
defines `memcpy`, `memset`, `memmove`, `memcmp` as strong external
symbols for the freestanding production build) is in
`PROD_C_FOR_TESTS` (`tawcroot/Makefile:127-138`) and gets linked into
the test binary too. So the hosted build ends up with both glibc's and
our own `mem*` definitions, resolved by static-link order — `libc.a`'s
copies appear last and only fill in unresolved references for cleat /
STC translation units; tawcroot TUs link against our local copies.

This works today, but is fragile:

- Reordering link inputs would silently flip which `memcpy` runs in
  the test orchestrator. A future `nm`-based check or
  `-Wl,--whole-archive libc.a` invocation could explode.
- A bug introduced in `strings.c::memcpy` (e.g. an alignment mistake)
  would only show up in production unless the test orchestrator
  exercises it through PROD_C_FOR_TESTS code paths.

## Fix options

1. **Suppress the `mem*` defs in hosted builds** — gate `memcpy` /
   `memset` / `memmove` / `memcmp` in `strings.c` behind
   `#if !__STDC_HOSTED__`. Hosted builds rely solely on glibc.
   Trade-off: `strings.c`'s versions are no longer covered by the
   hosted-glibc unit tests, so any divergence (esp. alignment edge
   cases) won't surface there.

2. **Static-link strings.c's defs.** Mark them `static` and rename to
   `tawc_memcpy` etc.; then have `tawc_string.h` `#define memcpy
   tawc_memcpy` in freestanding builds and pull `<string.h>` in hosted.
   Eliminates the link-order dependency entirely. Heavier touch but
   removes ambiguity.

3. **Document and lock the link order.** Cheapest: add a comment in
   `strings.c` and `Makefile` saying "the hosted build relies on
   `libc.a` being last in `TESTS_LDFLAGS` so glibc's mem* fill in
   cleat/STC's references and our local defs serve PROD_C_FOR_TESTS
   TUs." Plus a CI check that `nm $tests | grep ' T memcpy'` shows
   the local def, not glibc's.

## Severity

Pre-existing — predates the mem-builtin dedup that just landed
(which only added `memcmp` to the same set, following the existing
pattern). Not currently breaking anything, but the next person to
touch link options could trip on it without warning.
