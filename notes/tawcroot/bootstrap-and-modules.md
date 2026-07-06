# tawcroot — bootstrap, entry & module layout

## Bootstrap & entry

`tawcroot` is invoked from `TawcrootMethod.startInside`, which
builds an argv of the rough shape:

```sh
#!/system/bin/sh
exec /data/app/~~<hash>/lib/arm64-v8a/libtawcroot.so \
     -r /data/data/me.phie.tawc/distros/<id>/rootfs \
     -b /system:/system \
     -b /vendor:/vendor \
     -b /apex:/apex \
     -b /system_ext:/system_ext \
     -b /linkerconfig:/linkerconfig \
     -b /dev:/dev \
     -b /proc:/proc \
     -b /sys:/sys \
     -b /dev/binderfs:/dev/binderfs \
     -b /data/data/me.phie.tawc:/data/data/me.phie.tawc \
     -w /root \
     -- /usr/bin/bash -l "$@"
```

The `-b` set mirrors `ChrootMounter.mountScript`'s mount set + the
proot-method's mount set (`notes/proot.md`), including `/proc`,
`/sys`, `/dev`, and the libhybris Android system-library paths.
Since we don't actually *mount* anything, this is just bind-table
entries; the host paths get directly aliased into the rootfs view.
As in `ProotMethod`, bind sources that do not exist on a given
Android version (`/system_ext`, `/linkerconfig`, sometimes
`/dev/binderfs`) are filtered out when rendering the wrapper rather
than making tawcroot reject the whole argv.

**`/dev/shm` is emulated in-handler via `memfd_create`** — no host
directory bound, no flash-write cost. Path-bearing syscalls under
`/dev/shm/<name>` are intercepted by the SIGSYS handler and routed
to a small (name → memfd) table in `src/shm.c`:

- `openat(O_CREAT)` creates a `memfd_create(name, MFD_ALLOW_SEALING)`,
  stores an internal **non-CLOEXEC** dup at the high reserved-fd
  range, and hands a fresh dup to the guest. `O_EXCL` and `O_TRUNC`
  honored.
- `openat` on an existing name returns a fresh dup of the cached fd.
- `unlinkat` drops the name; the segment lives on as long as any
  fd is open (POSIX shm semantics, kernel refcount).
- `fstatat`/`statx`/`faccessat` synthesize sensible answers for both
  `/dev/shm` (synthetic dir) and `/dev/shm/<name>` (regular file
  with `fstat`-derived size, owner uid 0).

Cross-process visibility for fork+execve patterns (Mozilla parent →
content IPC) is preserved via two mechanisms:

1. The internal memfds are non-CLOEXEC, so they survive the
   handler-driven `execveat` re-exec verbatim — same fd numbers,
   same kernel objects.
2. `exec_state` ferries the (name, fd_int) pairs through the memfd
   handed across re-exec; `--exec-child`'s loader calls
   `tawcroot_shm_register` to rebuild the (name → fd) map without
   re-creating any kernel-side memfd.

Mozilla's IPC SHM is not contended in practice (a tiny spinlock
guards the table; held only across the create/unlink syscalls).
`mmap`/`ftruncate`/`mremap`/etc. operate on the returned fd as
real kernel operations — no further handler involvement.

Known low-value fidelity gaps vs. real `/dev/shm` (the
program-tripping ones — O_RDONLY access mode, per-open file offsets,
`.`/`..` classification, bogus STATX_SIZE — were fixed by re-opening
the internal memfd via `/proc/self/fd/<n>` with the guest's access
mode):

- `mode` is ignored (`(void)mode` in shm_open) and stat always
  synthesizes 0600 regardless of the creator's mode or later
  `fchmod`. Fine under fake-root.
- Handler inconsistency around the `/dev/shm` directory itself:
  `stat`/`statx`/`access` fake it as existing, but `openat`/`chdir`
  of it return ENOENT, and `truncate`/`utimensat`/`renameat*`/
  `statfs` of `/dev/shm/<name>` aren't intercepted at all. A
  configure-style probe sees a directory that stats but can't be
  opened.

`-w` sets initial CWD (translated). We also export a small set of
env vars (`HOME`, `USER`, `TMPDIR`, `PATH`) before exec.

## Module layout

Actual layout (flat — the `syscalls/` subdir was planned but the file
count never justified it; kept as one `syscalls_fs.c` until execve
adds an `exec.c`).

```
tawcroot/                            # everything tawcroot-specific lives here
├── README.md           # short: "see notes/tawcroot/"
├── build               # cross-ABI NDK build (also stages into APK jniLibs)
├── build-fixtures.sh   # NDK build for guest fixtures (loader smoke)
├── test.sh             # runs the cleat orchestrator (host) or pushes
│                       #   testhost via adb (device)
├── Makefile            # incremental host build (production + testhost + cleat tests)
├── include/                            # production headers — no test scaffolding
│   ├── tawcroot.h      # entry-point contract (tawcroot_main)
│   ├── arch.h          # syscall_args struct, includes arch/<arch>.h
│   ├── arch/{aarch64,x86_64}.h  # arch_read_args / arch_write_return
│   ├── chroot.h        # chroot(2) emulation registration
│   ├── dirent_filter.h # getdents64 reserved-fd dirent compaction — pure
│   ├── dispatch.h      # syscall→handler table API
│   ├── errno_neg.h     # negated-errno constants (TAWC_EINVAL == -22)
│   ├── exec_handler.h  # execve handler entry (memfd build + execveat)
│   ├── exec_state.h    # serialized re-exec state struct + (de)serialize
│   ├── fdtab.h         # reserved high-fd table + close/dup protection
│   ├── filter.h        # seccomp filter install API
│   ├── filter_build.h  # pure cBPF program builder
│   ├── handler.h       # SIGSYS handler install + observation
│   ├── identity.h      # virtual identity struct + registration
│   ├── io.h            # libc-free print helpers + tawc_str_* builders
│   ├── loader_elf.h    # phdr parsing, image bounds, interp pointer
│   ├── loader_exec.h   # --exec-child entry + shebang resolution
│   ├── loader_jump.h   # asm-only stack-pivot + final jump to ld.so/_start
│   ├── loader_map.h    # mmap/mprotect of PT_LOADs, AT_PHDR computation
│   ├── loader_stack.h  # synthesize argv/envp/auxv on a fresh stack
│   ├── path.h          # translator, modes, bind table, open_in_view
│   ├── path_oracle.h   # readlink oracle interface used by resolver
│   ├── path_orchestrate.h # fold→bind→memo→resolve→bind ctx + memo struct
│   ├── path_resolve.h  # symlink walker — operates against an oracle
│   ├── path_scratch.h  # handler-safe PATH_MAX scratch-buffer pool
│   ├── proc_rewrite.h  # /proc/self/maps reverse-translation — pure
│   ├── proc_shadow.h   # /proc shadow-fd synthesis + /proc/self classify
│   ├── raw_sys.h       # tawc_<syscall> wrappers
│   ├── shm.h           # /dev/shm emulation (memfd-backed name table)
│   ├── signal_shadow.h # guest SIGSYS sigaction/sigmask virtualization
│   ├── supervisor.h    # shared per-process bootstrap (prod + --exec-child)
│   ├── syscalls_{control,exec,fs,socket}.h # handler registration entries
│   ├── sysnr.h         # per-arch syscall numbers
│   ├── tawc_string.h   # memcpy/memset/... freestanding-vs-hosted switch
│   ├── tawc_uapi.h     # kernel ABI constants (O_*, AT_*, struct stat, ...)
│   └── usercopy.h      # process_vm_readv-based guarded copy
├── src/                                # production sources — no test scaffolding
│   ├── main.c          # production entry: CLI parse, --exec-child dispatch
│   ├── chroot.c        # chroot(2) emulation — root-view swap on guest call
│   ├── dirent_filter.c # getdents64 compaction — pure
│   ├── dispatch.c      # syscall→handler table storage
│   ├── exec_handler.c  # execve guest-side entry (build memfd state + execveat)
│   ├── exec_state.c    # exec-state (de)serialization — pure
│   ├── filter.c        # install BPF program (seccomp + stub address)
│   ├── filter_build.c  # build BPF program — pure
│   ├── handler.c       # sigsys_handler dispatch, ucontext glue
│   ├── identity.c      # stateful virtual identity handlers
│   ├── io.c            # io.h print impl (via tawc_write)
│   ├── strings.c       # pure libc-free str/mem helpers + bounded builders —
│   │                   #   also linked into the cleat test runner under hosted
│   │                   #   glibc (tawcroot/tests/unit/test_strings.c)
│   ├── path.c          # translate(), reverse-translate, bind table, memo,
│   │                   #   open_in_view
│   ├── path_fold.c     # absolute-path folder (`.`/`..`/empty/`//`) — pure
│   ├── path_orchestrate.c # fold→bind→memo→resolve→bind staging, binds_reanchor — pure
│   ├── path_resolve.c  # symlink walker — pure, oracle-driven
│   ├── path_scratch.c  # scratch-buffer pool (CAS acquire/release)
│   ├── proc_rewrite.c  # /proc/self/maps line rewriter — pure
│   ├── proc_shadow.c   # /proc shadow memfds (maps/overflow/pci) + classify
│   ├── shm.c           # /dev/shm emulation
│   ├── signal_shadow.c # SIGSYS sigaction/sigmask shadow state
│   ├── supervisor.c    # shared bootstrap: rootfs fd, binds, handler, masks
│   ├── loader_elf.c    # ELF phdr parsing
│   ├── loader_map.c    # PT_LOAD mmap/mprotect, AT_PHDR computation
│   ├── loader_stack.c  # synthesize argv/envp/auxv on a fresh stack
│   ├── loader_exec.c   # --exec-child main: load guest + jump, shebang chain
│   ├── loader_io_prod.c # production loader I/O vtable (raw syscalls)
│   ├── syscalls_{fs,fd,control,exec,socket}.c # per-syscall handlers
│   ├── usercopy.c      # process_vm_readv probe + guarded guest copies
│   └── arch/{aarch64,x86_64}_{stub,loader_jump}.S  # _start, raw syscall stub,
│                                                   # sigreturn trampoline, loader jump
└── tests/                              # everything that isn't shipped in production
    ├── testhost/
    │   ├── include/
    │   │   ├── child.h     # --exec-child entry
    │   │   ├── rootfs_smoke.h # rootfs syscall smoke entry
    │   │   └── smoke.h        # foundation smoke harness
    │   └── src/
    │       ├── testhost_main.c  # argv dispatch (--exec-child / -r ROOTFS / foundation)
    │       ├── child.c          # --exec-child re-entry
    │       ├── rootfs_smoke.c   # rootfs syscall smoke (inline-asm probes)
    │       └── smoke.c          # foundation smoke (trap-contract, raw-syscall exercise)
    ├── unit/                # cleat-direct pure-function tests (no fork)
    │   └── test_strings.c   # tawc_strlen/streq/starts_with/parse_long/int_to_str
    ├── handler/             # cleat tests that fork tawcroot-testhost
    │   ├── steps.{c,h}      # parse [ok ]/[FAIL] lines from testhost stdout and
    │   │                    #   register one cleat test per check (dynamic)
    │   ├── test_foundation_smoke.c      # one dynamic test per foundation step
    │   └── test_rootfs_syscalls_smoke.c # builds fake rootfs, then rootfs smokes
    └── integration/         # cleat tests that fork production tawcroot
        └── programs/        # tiny C guests built by the runner and exec'd under tawcroot
```

The directory is laid out so it could be lifted into its own repo (no
external paths inside `tawcroot/`). The one tawc-app coupling is in
`tawcroot/build.sh`, which stages the production binary into
`app/src/main/jniLibs/<abi>/libtawcroot.so` for APK packaging,
and in `tawcroot/test.sh --device` which sources `scripts/lib/select-device.sh`
to pick the adb target — strip those if splitting.

Build artifacts (per `tawcroot/build.sh`):
- **Production tawcroot** — one static non-PIE ET_EXEC per ABI:
  `libtawcroot.so` for arm64-v8a / x86_64, `tawcroot` for host.
  Shipped as a jniLib like `libproot-loader.so` for the same
  APK-execve reason. No test scaffolding, no `--run-test`, no
  smoke driver, no third-party deps.
- **`tawcroot-testhost`** — same source set + `tawcroot/tests/testhost/src/`,
  compiled with `-DTAWCROOT_TESTHOST`. Built by default for
  `--abi=host`; built for cross-ABIs only with `--testhost`. Not
  packaged into the APK.
- **`tests`** — cleat orchestrator. Built by default for `--abi=host`
  (hosted glibc); cross-compiled for `aarch64`/`x86_64` against bionic
  on demand via `tawcroot/build.sh --abi=<abi> --tests`. The Android
  variant is what `tawcroot/test.sh --device` pushes and runs as adb
  shell; same four-layer suite, same filter syntax, same exit code as
  host mode — only the orchestrator binary changes.

