# Usecase test: SQLite with WAL and concurrency

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a user's app/scripts keep data in SQLite — the single most common "database on a phone" case.

WAL mode is the interesting part: it uses an mmap'd `-shm` sidecar file
and POSIX advisory locks, both of which route through tawcroot's file
handling in ways plain reads/writes don't.

## Prerequisites

- Cache proxy up (README step 6). `pacman -S --noconfirm sqlite`.

## Steps

Work under `/root/usecase-sqlite/`.

1. Create a DB, `PRAGMA journal_mode=WAL;` (verify it reports `wal`, not
   a silent fallback to `delete`), create a table, insert 10k rows in a
   transaction, run aggregate queries, verify counts.
2. Concurrency: run a writer loop (`INSERT` batches in one process) while
   a second process does repeated `SELECT count(*)`. Both should proceed;
   brief `SQLITE_BUSY` retries are fine, corruption or deadlock is not.
3. `PRAGMA integrity_check;` → `ok`.
4. Crash-ish recovery: kill -9 the writer mid-run, reopen the DB, run
   `integrity_check` again, confirm the WAL is recovered.
5. Repeat step 1 briefly with `journal_mode=DELETE` for the non-WAL path.

## Expected results

- WAL mode actually engages; concurrent reader/writer work; integrity
  checks pass, including after a killed writer.

## Known issues / caveats

- If WAL silently degrades or `-shm` mapping fails, that's a tawcroot
  finding (mmap on files, `/dev/shm` is separately emulated via memfd —
  notes/tawcroot/status.md); capture `strace`-level detail if you can.

## Cleanup

Remove `/root/usecase-sqlite/`, `pacman -Rns sqlite` (skip removal if it
was already installed as a dependency — check before removing).
