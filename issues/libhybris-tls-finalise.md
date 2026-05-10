# libhybris TLS finalisation: remaining audit items

The TLS architecture fix landed in libhybris commit `4ad3dea`
("linker_tls: register dynamic, lazy-promote on IE/TLSDESC, init
.tdata per-thread"), bumped on tawc in `80b213c`. The worktree is
clean and `TAWC_FORK.md` documents the new design. This issue tracks
the open follow-up items — gaps the architecture fix didn't close, or
audit work to confirm we don't ship dead/load-bearing code.

## Latent gaps in current code

### `init_ptr` lifetime in `g_promoted_tls`

`hooks.c::hybris_promoted_tls.init_ptr` is a raw pointer into the
promoted `.so`'s TLS segment. Nothing copies the init image into
hybris-owned storage. If `hybris_dlclose` actually unmaps the
segment, the catch-up-replay path in `_hybris_hook___get_tls_hooks`
reads dangling memory.

`repro.c::run_post_dlclose_replay_check` now demonstrates the bug:
after `hybris_dlclose` of a promoted TLS module, a fresh thread that
calls `_hybris_hook___get_tls_hooks` during catch-up replay segfaults
while reading the stale init pointer. This is a true libhybris
failure, not a test bug.

Fix: memdup the init image into the registry entry and free it never
(process-lifetime, same as `g_promoted_tls.entries`), or make
promoted TLS modules effectively `NODELETE` so the source mapping
cannot disappear.

### Bounds check uses `init_size` (filesz), not segment `size` (memsz)

`hybris_apply_static_tls_locked` only checks
`static_offset + init_size <= BIONIC_STATIC_TLS_SIZE`. The patcher's
tp-relative reads of `.tbss` vars land at offsets in
`[init_size, segment.size)`. If a future vendor `.so` has a memsz
that pushes those reads past the array, the `abort()` won't fire —
silent OOB into whatever IE TLS sits next to `tls_static_tls` in the
static-TLS layout.

Fix: also bounds-check `static_offset + segment.size`. Pass `memsz`
through `_hybris_init_static_tls_for_thread` (or compute via the
linker side and pass alongside `init_size`).

## Audit items

### Catch-up-replay invariant in `_hybris_hook___get_tls_hooks`

A glibc-spawned worker that only does TLSDESC reads against bionic
`.so`s and never calls a hooked libc function never enters
`_hybris_hook___get_tls_hooks`, so it never replays the registry and
reads zeros for non-zero `__thread` initialisers. Rare in practice
(bionic code paths almost always interleave TLS access with
errno/locale/pthread-key calls).

- [ ] Add a comment in `hooks.c` documenting this invariant ("every
  thread that ever runs bionic-side TLS-using code first passes
  through `__get_tls_hooks`; if that ever stops being true, the
  catch-up replay needs a different trigger").
- [ ] Optional: hook `pthread_create` to seed
  `tls_module_init_count = g_promoted_tls.count` for the new thread.
  Out-of-scope unless a test catches the gap.

### Generation bumps in `unregister_tls_module`

Both branches bump `libc_modules.generation` /
`__libc_tls_generation_copy` / `*generation_libc_so`. Under the
static-only model, the consumers (`update_tls_dtv` →
`tls_get_addr_slow_path`) are unreachable on aarch64+libhybris.
Possibly load-bearing if apex `libc.so`'s own DTV cache reads the
generation copy.

- [ ] Drop the bumps, run the integration suite plus
  `libhybris-tls-repro` stress loop on each device. If still passes,
  drop is safe; if anything regresses, restore and document why.

### Eager vs lazy `bionic_tcb` reservation

Two viable shapes:
- **Lazy (current).** First `promote_tls_module_to_static` reserves
  `bionic_tcb` at offset 0; `tls_tp_base` is hardcoded `16` in
  `linker.cpp::soinfo::relocate` because `relocate()` reads the value
  once at the top, before the first reloc handler triggers
  reservation.
- **Eager.** Reserve `bionic_tcb` once in `android_linker_init()`
  before any `relocate()` runs. Drops `g_bionic_tcb_reserved` and the
  `tls_tp_base = 16` hardcode (becomes `offset_thread_pointer()`).

Eager is structurally simpler and removes our-added dead code. Lazy
stays closer to upstream's mental model (layout reserved on demand)
and is what's currently shipping with a verbose comment.

- [ ] Pick one, update `TAWC_FORK.md`'s "Linker bionic-TLS
  bookkeeping" paragraph to match if we change.

## Hardening

### q.so symbol-collision hardening

q.so exports 467 symbols at default visibility. Three collide with
host glibc/ld.so names: `__tls_get_addr`, `strlcpy`, `strlcat`.
`__tls_get_addr` was the active hazard on Pixel 4a — under the new
fix it's unreachable (the dynamic TLSDESC slow path is dead in our
flow), but a future regression that re-introduces a self-PLT call
would silently bind to glibc's `__tls_get_addr` again.

- [ ] Add `-fvisibility=hidden` to `q_la_CPPFLAGS` in
  `hybris/common/q/Makefile.am`, with explicit `default` visibility
  on the public q.so API surface (the `android_*` / `__loader_*`
  namespace + anything libhybris-common.so calls into). Verify
  q.so's exported set drops from ~467 to ~50 with `nm -D`. Fall back
  to a version script if the Makefile flag is too invasive.

### Heap-indirection escape hatch for `BIONIC_STATIC_TLS_SIZE`

1024 B (944 B usable) fits apex libc + Adreno + GTK/firefox stack on
4a + OP9. If a bigger vendor stack overruns it, the documented
escape hatch is: switch `tls_static_tls` from inline IE `char[]` to
IE `char* heap_block`, allocate per-thread on first
`__get_tls_hooks`, add one extra `LDR` to the patcher thunk to
dereference. Cost: thunk grows 16→20 B, `count_tls_patches` /
`THUNK_SIZE` follow.

- [ ] Pixel Fold (when available) re-test of `libhybris-tls-repro`
  + integration suite. If fits, no action. If overruns, implement
  heap indirection rather than punting.

## Tests to add

The existing repro covers value correctness, per-thread isolation,
post-dlclose replay (currently failing on the stale `init_ptr` bug),
and stress. Missing:

- [ ] `.tbss` covers a non-trivial `memsz - filesz` gap (the current
  `g_tls_pad[32]` test exercises this but only at filesz; add a
  `__thread char tbss_block[256];` with no initialiser to stress the
  memsz path).
- [ ] Asserts that q.so's exported set after the visibility change
  does not include `__tls_get_addr` (or reduces to the documented
  ~50-symbol public API surface).

## Landing

Each item above is independent enough to ship in its own libhybris
commit. When all are resolved:

- Re-tag libhybris (`tawc-DD-Mon-YYYY-N`), push tag, bump
  `deps/deps.list` on tawc.
- Update `TAWC_FORK.md`'s "Linker bionic-TLS bookkeeping" section if
  any of the audit items change semantics (eager TCB reservation,
  bounds-check fix, init-image ownership, visibility hardening).
- Delete this file.
