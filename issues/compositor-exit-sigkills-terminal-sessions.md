# Compositor "Exit" notification SIGKILLs unrelated terminal sessions

`CompositorService.exitFromNotification` calls
`ProcessScanner.killAllKnownRootfs`, which SIGKILLs every guest process
in every rootfs (deliberately no SIGTERM pass — see
`ProcessScanner.kt`). That includes in-app terminal shells and their
jobs, which per `TerminalActivity`'s own docs have no compositor
involvement. The notification frames the action as compositor-scoped
("TAWC running / N Linux windows open" with an Exit button), so a user
exiting with zero windows open loses e.g. a background compile in a
terminal tab — uncleanly, with no prompt.

Options, pick one:
- Scope the kill: exclude pids owned by live `TerminalSessions` (and
  their descendants), or kill only processes holding the Wayland/X11
  sockets.
- Keep "quit everything" semantics but say so: reword the notification
  action, and/or confirm when terminal sessions are alive.

Found in the 2026-07 production-readiness sweep (agent audit, verified
against source).
