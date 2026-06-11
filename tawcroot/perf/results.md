# Perf baselines

Recorded runs of `run-perf.sh` so future runs can be compared against
something. Rerun with the same config and append a new section; flag
anything that moves a lot (say >25% on a median) without a known cause.
Raw TSVs live in `build/tawcroot-perf/` (not committed).

Medians are the number to compare; see README.md "Reading results".

## 2026-06-10 host x86_64

- Host: AMD Ryzen AI 9 HX 370, dev laptop.
- Config: `--iterations 50000 --rounds 20 --interleave`, minimal
  rootfs (empty dir + `tmp/`), static musl bench binary.
- tawcroot: host build @ git 164f80b (+ working tree).
- proot v5.3.1-99a84175 (seccomp accelerator on), runsc
  release-20260520.0 rootless, both from `build/tawcroot-perf/tools/`.
- chroot mode skipped (no root).

Median ns_per_op:

| benchmark           | native  | tawcroot | proot   | gvisor    |
|---------------------|--------:|---------:|--------:|----------:|
| getpid              |      73 |      126 |      80 |       958 |
| stat                |     352 |    2,285 |  17,109 |     5,268 |
| open_read_close     |     552 |    2,300 |   7,879 |    13,814 |
| readdir_entries     |      43 |       60 |      90 |       238 |
| create_write_unlink |   2,880 |    6,454 |  16,138 |    27,893 |
| fork_exec_wait      | 165,430 |  407,786 | 379,614 | 1,476,076 |

Expected shape, for spotting regressions:

- Path syscalls (`stat`, `open_read_close`, `create_write_unlink`):
  tawcroot ~2-7x native and 2.5-7x faster than proot. This is the
  core win of SIGSYS dispatch over ptrace; if tawcroot drifts toward
  proot here, something in the handler hot path regressed.
- `fork_exec_wait`: tawcroot ~2.5x native, slightly (~7%) behind
  proot. Known structural cost: each guest execve re-execs tawcroot
  itself and rebuilds runtime state in the child, vs proot's
  long-lived tracer + tiny loader stub. Was 476µs in a May 2026 run
  with the same config; improvements here are real, big increases
  are regressions in the exec/state-handoff path.
- `getpid`: untrapped; measures BPF filter evaluation only
  (~50ns over native from the IP-allowlist prelude + linear JEQ
  chain). Sanity check, not a comparison.
- `readdir_entries`: near-native for everyone; tawcroot pays a
  little for dirent filtering.
