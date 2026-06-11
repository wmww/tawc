# tawcroot perf harness

Standalone microbenchmarks for comparing:

- `native` host execution against a rootfs path prefix
- `tawcroot -r ROOTFS -- CMD`
- `proot -r ROOTFS CMD`
- real `chroot ROOTFS CMD`
- gVisor `runsc` with the rootfs as an OCI bundle root

This is not part of `tawcroot/test.sh` or the integration suite. It is
for local performance runs only.

Past results are recorded in [results.md](results.md); compare new runs
against those baselines and append significant runs there.

## Quick start

Build host tawcroot if needed:

```sh
tawcroot/build.sh --abi=host
```

Run the available backends:

```sh
tawcroot/perf/run-perf.sh --rootfs /path/to/rootfs
```

Run a focused comparison:

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes native,tawcroot,proot \
  --iterations 50000 \
  --rounds 5
```

The runner writes TSV to `build/tawcroot-perf/results-*.tsv` and prints
mean/median/min/max `ns_per_op` summary rows.

## Local competitor tools

These tools are only for local perf runs. Do not add them to the normal
app build.

For x86_64 hosts, install local `proot` and `runsc` binaries under
`build/tawcroot-perf/tools/`:

```sh
mkdir -p build/tawcroot-perf/tools
cd build/tawcroot-perf/tools

gvisor_base="https://storage.googleapis.com/gvisor/releases/release/latest/$(uname -m)"
curl -fsSLO "$gvisor_base/runsc"
curl -fsSLO "$gvisor_base/runsc.sha512"
sha512sum -c runsc.sha512
chmod 0755 runsc

curl -fL -o proot https://proot.gitlab.io/proot/bin/proot
chmod 0755 proot
```

Then pass them explicitly:

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes native,tawcroot,proot,gvisor \
  --proot build/tawcroot-perf/tools/proot \
  --runsc build/tawcroot-perf/tools/runsc \
  --iterations 50000 \
  --rounds 20 \
  --interleave
```

The older GitHub `proot-static-build` v5.1.1 binaries segfaulted on the
current dev host during testing. Use the current upstream PRoot binary
unless you specifically want to compare against that old build.

## Requirements

- A Linux rootfs matching the machine architecture.
- A static C compiler path, usually `musl-gcc` or a host `cc` with
  static libc available. Alternatively pass `--bench-bin PATH`.
- `build/tawcroot-host/tawcroot` for the `tawcroot` backend, or pass
  `--tawcroot PATH`.
- Optional: `proot` for the `proot` backend.
- Optional: `runsc` for the `gvisor` backend.
- Optional: root, or `--sudo-chroot`, for the `chroot` backend.

For rootless gVisor, the rootfs path must be traversable by the current
user. A disposable test rootfs should normally have at least:

```sh
chmod 0755 /path/to/rootfs
chmod 1777 /path/to/rootfs/tmp
```

The benchmark binary is copied into:

```text
<rootfs>/tmp/tawcroot-perf/tawcroot-perf
```

The workload creates files under:

```text
<rootfs>/tmp/tawcroot-perf/work
<rootfs>/tmp/tawcroot-perf/create
```

## Workloads

- `getpid`: baseline syscall loop.
- `stat`: repeated path metadata lookups.
- `open_read_close`: repeated path open/read/close.
- `readdir_entries`: repeated directory scans.
- `create_write_unlink`: create/write/delete cycle.
- `fork_exec_wait`: small fork/exec/wait loop.

These are microbenchmarks, not application benchmarks. Use them to spot
large syscall-path regressions, then validate with real workloads such
as package manager operations or browser startup.

## Reading results

Prefer `median` over `mean` when comparing backends. `min`/`max` are
printed so scheduler and CPU-frequency noise is visible.

Use at least `--rounds 20` for numbers worth discussing. If a result
looks surprising, rerun with a different backend order, for example:

```sh
tawcroot/perf/run-perf.sh \
  --rootfs /path/to/rootfs \
  --modes proot,tawcroot,native,gvisor \
  --proot build/tawcroot-perf/tools/proot \
  --runsc build/tawcroot-perf/tools/runsc \
  --iterations 50000 \
  --rounds 20
```

Treat `getpid` as a sanity check, not a rootfs benchmark. Some backends
may fast-path it without exercising path translation.

The runner always executes all rounds for one backend before moving to
the next backend unless `--interleave` is passed. Backend-major order is
simple and reproducible, but it can preserve cache and CPU-frequency
ordering effects. For careful comparisons, use `--interleave`, compare
at least two backend orders, or inspect the raw TSV.
