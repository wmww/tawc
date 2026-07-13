# Task manager su kill can SIGKILL a recycled pid

`ProcessScanner.stop` scans, sends SIGTERM, waits up to 1 s, then
SIGKILLs by the same pid. For `requiresSu` rows the kill runs as root
(`SuProcfsScanner.kill`), and `waitForExit` treats `EPERM` as "still
alive" — so if the root-owned guest exits during the grace window and
the kernel recycles the pid, the app-uid liveness probe reads EPERM
("alive") and the root SIGKILL lands on an arbitrary process,
potentially a system one. The `killAllInRootfs` sweeps have the same
scan→kill gap.

Only reachable with chroot installs (the su path), which are
debug-only — the app-uid `Os.kill` path can only ever hit same-uid
processes. Fix before chroot ever ships in release: re-verify identity
(e.g. `/proc/<pid>/stat` start time captured at scan, re-read before
SIGKILL) instead of trusting the pid across the grace window.

Found in the 2026-07 production-readiness sweep.
