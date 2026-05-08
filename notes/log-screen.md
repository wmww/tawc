# Operations log screen + ops/ package

`me.phie.tawc.ops` is the generic abstraction layer for "long-running
work that needs progress + a log + maybe a cancel button." It's
shared between install / uninstall (today) and any future broker-
driven action (tomorrow) — the panel, the activity, the registry, and
the per-op notifications all speak only the [Operation] interface, so
adding a new kind of operation doesn't touch any UI code.

## Pieces

| File | Role |
| --- | --- |
| `Operation.kt`                  | Interface: `id`, `title`, `progress: StateFlow`, `log: SharedFlow`, `cancelConfirmation`, `cancel()`. The single contract every viewer / notifier speaks. |
| `OperationProgress.kt`          | Generic `OperationStage` (`IDLE / RUNNING / DONE / FAILED`) + `OperationProgress`. Concrete pipelines carry their own finer-grained stage enum (`InstallStage`) and project down at the [Operation] boundary. |
| `OperationsRegistry.kt`         | Application-singleton `Map<id, Operation>` exposed as a StateFlow. The single source of truth for "what's running right now." |
| `OperationLogPanel.kt`          | Reusable view: bold status line that flips success-green/danger-red on terminal, accent-tinted progress bar, scrolling log, subdued borderless Cancel. Bound to an [Operation] via `bind(op)`. Keeps its last-rendered state on `unbind` so a viewer reading a terminated op sees the frozen final state. |
| `LogScreenActivity.kt`          | Generic per-op viewer. Takes `--es operationId <id>`, looks up the op in the registry, and binds the panel. **Pure viewer** — opening it never side-effects. Cold open against a no-longer-registered id shows a "no operation in flight" placeholder. |
| `OperationsNotificationCenter.kt` | App-singleton that mirrors the registry into ongoing notifications on the `tawc-operations` channel. One notification per registered op; tap → `LogScreenActivity` for that op; cancel-action → `CancelOperationReceiver`. Also exposes `fgsAnchorFor(opId)` so a service can `startForeground` against a per-op notification. |
| `CancelOperationReceiver.kt`    | Manifest receiver (`exported=false`) for the notification's Cancel action. Looks up the op in the registry and calls `op.cancel()`. **Bypasses `cancelConfirmation`** — a notification-action tap is itself the deliberate decision. |

## Lifecycle conventions

- **Register** in [OperationsRegistry] at the moment work begins; for
  service-backed ops that's right before the worker coroutine launches.
- **Unregister** in the worker's `finally`. Per the project's
  "unregister immediately on terminal" choice, terminal Operations
  vanish from the registry the moment the work ends; viewers that
  were bound at the moment of terminal keep their frozen view, but a
  fresh open finds nothing. There is no "completed runs" history at
  this layer — the host TTY exit code (via the broker) and the in-app
  notification's brief lifetime are the surfaces.
- **The notification is automatic.** Once the op is in the registry,
  the notification center observes its progress and posts /
  updates / cancels the notification with no per-op code.

## Adding a new kind of operation

1. Implement [Operation], or use the default [me.phie.tawc.ops.MutableOperation]
   — a thin adapter with a freely-mutable progress StateFlow plus a
   caller-supplied log SharedFlow + cancel handler. [InstallationService]
   uses it for install/uninstall jobs and refused-by-gate transients;
   [me.phie.tawc.dev.BrokerOpMirror] uses it for the broker's
   OP_TITLE log-screen path.
2. Register it in [OperationsRegistry] when the work starts;
   unregister + close in `finally`.
3. (Optional) Surface from CLI by writing a [BrokerAction] handler in
   `me.phie.tawc.dev`-shaped code — `me.phie.tawc.install.InstallActions`
   is the template. Register from `TawcApplication.onCreate`.
4. (Optional) If the work needs to outlive any view (e.g. a several-
   minute install), back it with an FGS so the OS doesn't reap the
   process when no activity is bound. Use
   `OperationsNotificationCenter.fgsAnchorFor(opId)` for the
   `startForeground` call so the per-op notification doubles as the
   FGS anchor.

The viewer / notification / cancel-receiver / broker action surfaces
all auto-pick up the new op — no UI code to write.

## Why "unregister on terminal" instead of "linger"?

We considered keeping terminal ops in the registry until overwritten
so a re-opened LogScreenActivity could show the frozen final state.
Decided against it because:

- The host TTY (broker action) already keeps terminal output for as
  long as the user wants — that's what the shell is for.
- The in-app log screen, if still open at terminal, keeps its frozen
  view via `OperationLogPanel.unbind` not clearing the views.
- Lingering ops would need an explicit clear policy ("when does it go
  away?"); none of the obvious answers (timeout / on next op / on
  user-tap) felt clearly better than just "it's done; move on."

If a "completed runs" history view becomes interesting, it's a
separate file (rotated logs of finished operations), not a
modification to the live registry.
