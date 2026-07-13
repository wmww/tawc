# OpenSSH sshd cannot serve on tawcroot — seccomp sandbox EPERM kills the preauth child

Found by the `cli-ssh-server` usecase test on the physical OnePlus
(Android 14, device 50f4ca18), Arch tawcroot install, 2026-07-13.
openssh-10.4p1-2, OpenSSL 3.6.3.

## Symptom

`sshd` installs, `ssh-keygen -A` generates host keys, and
`/usr/sbin/sshd -D -e -p 2222` binds and listens fine:

```
Server listening on 0.0.0.0 port 2222.
Server listening on :: port 2222.
```

But the moment a client connects, the connection is reset during key
exchange:

```
$ ssh -p 2222 -i /root/.ssh/usecase_key root@127.0.0.1 'echo hi'
kex_exchange_identification: read: Connection reset by peer
Connection reset by 127.0.0.1 port 2222
```

The listener stays alive; only the per-connection child dies. The `-e`
log gives the exact cause:

```
ssh_sandbox_child: prctl(PR_SET_SECCOMP): Operation not permitted [preauth]
```

sshd's preauth privsep child unconditionally installs a seccomp-BPF
sandbox via `prctl(PR_SET_SECCOMP, ...)`. tawcroot **denies guest
seccomp installation** (by design — see below) and returns `EPERM`.
Modern sshd treats sandbox setup failure as fatal, so the child aborts
before the SSH banner exchange completes → "Connection reset".

## No runtime workaround in openssh 10.x

The plan suggested `-o UsePrivilegeSeparation=no`. That option was
removed years ago; 10.4p1 only warns and ignores it:

```
command-line line 0: Deprecated option UsePrivilegeSeparation
```

Privsep is mandatory and the sandbox type is a **compile-time** choice
(`--with-sandbox`); Arch builds `seccomp_filter`. `sshd -T` exposes no
`sandbox`/`privsep` config key. So there is no config or CLI flag that
disables the seccomp sandbox on a stock Arch openssh — it cannot serve
a session on tawcroot as shipped.

## Layer

tawcroot. `notes/tawcroot/status.md` (roadmap item 6, "Guest `SIGSYS`
and seccomp virtualization") states the intended behaviour is to
"**deny guest seccomp installation**". The current denial returns
`EPERM`, which is correct for the design but incompatible with any
guest that hard-fails on sandbox setup (sshd, and likely other
seccomp-sandboxing daemons). Firefox tolerates the EPERM; sshd does
not.

## The usecase still works — via dropbear

This is a **partial pass** per the plan's expected results. dropbear
(2026.91-1) serves fine on the same rootfs, proving sockets and the
fake-root login path are healthy and the failure is specific to
openssh's seccomp sandbox:

- Loopback: `ssh -p 2222 -i usecase_key root@127.0.0.1 'uname -a; id'`
  → `over-ssh-dropbear`, correct `uname`, `uid=0(root)`. Dropbear log
  shows `Pubkey auth succeeded for 'root' with ssh-ed25519 key ... from
  127.0.0.1`.
- Host-side via `adb forward tcp:2222 tcp:2222`: `ssh` from the host
  machine reached the phone's Linux (`host-reached-dropbear`,
  `uid=0(root)`). Inbound reachability from outside the app works.
- Dropbear `-R` generated its host key on the fly; it reuses
  `/root/.ssh/authorized_keys`.

Negative check (expected, not a bug): binding privileged port 22 fails
for **both** servers — no `CAP_NET_BIND_SERVICE`:

```
dropbear: Failed listening on '22': Error listening: Permission denied
sshd:     Bind to port 22 on 0.0.0.0 failed: Permission denied.
```

## Fix options

1. **Make the guest seccomp deny non-fatal for callers.** Instead of
   `EPERM`, tawcroot could return **success without installing**
   anything (a no-op fake-accept) for `prctl(PR_SET_SECCOMP)` /
   `seccomp(SECCOMP_SET_MODE_FILTER)`. sshd's sandbox would "succeed",
   install nothing, and continue. Risk: a guest relying on the sandbox
   for real confinement gets none — but tawcroot already sits under its
   own filter, and pretending a no-op sandbox is installed is a common
   compatibility shim (proot does similar). This is the most impactful
   fix and would unblock openssh and other sandboxing daemons.
2. Ship/patch an openssh built with `--with-sandbox=no` (or `rlimit`).
   Only helps openssh, requires a custom package — not attractive.
3. Document dropbear as the supported SSH server on tawcroot and leave
   openssh as known-broken. Lowest effort; dropbear covers the usecase.

Recommendation: option 1 if we want the marquee "sshd into your phone"
story to work with stock openssh; otherwise option 3 + a doc note.

## Repro

```
TAWC_INSTALL_ID=arch scripts/rootfs-run.sh 'pacman -S --noconfirm openssh; ssh-keygen -A'
# start:  setsid nohup /usr/sbin/sshd -D -e -p 2222 >/root/sshd.log 2>&1 &
# connect (from another rootfs-run session):
#   ssh -p 2222 -i /root/.ssh/<key> root@127.0.0.1 true
# → Connection reset; /root/sshd.log shows
#   ssh_sandbox_child: prctl(PR_SET_SECCOMP): Operation not permitted [preauth]
```
