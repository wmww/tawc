# Audio Bridge Plan

This is the planned direction for Linux app audio in tawc. No production
audio bridge exists yet.

## Goal

Linux desktop apps should be able to play sound, and later record from the
phone microphone, while keeping the Android side in control of Android audio
permissions, lifecycle, routing, and app-private state.

The rootfs side should look like a normal Linux desktop audio environment:

- native PipeWire clients should work.
- PulseAudio clients should work through `pipewire-pulse`.
- ALSA-only clients should work through the PipeWire ALSA plugin.
- JACK clients are a later compatibility target through PipeWire's JACK
  libraries, not a first milestone.

The Android side should not expose fake `/dev/snd/*` devices. Android app
audio belongs behind `AudioTrack`/`AudioRecord` or AAudio/Oboe, with the normal
Android permission and lifecycle rules.

## Preferred Direction: PipeWire Over Pipe Tunnel

Prototype a PipeWire-first audio stack in the rootfs and connect it to tawc
with `libpipewire-module-pipe-tunnel`.

Steady-state playback:

```text
Linux app
  -> native PipeWire OR pipewire-pulse OR pipewire-alsa
  -> pipewire daemon
  -> libpipewire-module-pipe-tunnel sink
  -> /usr/share/tawc/audio-out-0
  -> tawc Android audio bridge
  -> AudioTrack or AAudio
```

Steady-state capture:

```text
AudioRecord or AAudio
  -> tawc Android audio bridge
  -> /usr/share/tawc/audio-in-0
  -> libpipewire-module-pipe-tunnel source
  -> pipewire daemon
  -> native PipeWire OR pipewire-pulse clients
```

`/usr/share/tawc/` is already the shared namespace for compositor-controlled
runtime endpoints such as Wayland and Kumquat sockets. Audio endpoints should
fit the same model: the app owns the real files under its app-private `share/`
directory, and each install method exposes that directory inside the rootfs at
`/usr/share/tawc/`.

`libpipewire-module-pipe-tunnel` is loaded into the `pipewire` daemon; it is
not another process. The module can expose:

- a `sink` node: samples played to the PipeWire sink are written to a pipe.
- a `source` node: samples read from a pipe are exposed as a PipeWire source.

Start with fixed-format PCM:

- playback: `s16le`, stereo, 48000 Hz.
- capture: `s16le`, mono or stereo, 48000 Hz; mono is enough for a first mic
  bridge.

PipeWire can resample and remix for clients. tawc should avoid accepting a
large format matrix until fixed-format playback and capture are stable.

## Process Model

Recommended prototype process set:

```text
rootfs:
  pipewire
  wireplumber
  pipewire-pulse

Android app:
  tawc process with audio bridge threads
```

`pipewire` is the graph/server and loads `libpipewire-module-pipe-tunnel`.

`wireplumber` is the session/policy manager. It should create defaults and link
streams to the tawc pipe sink/source. It may be possible to replace it with
static/manual links for a controlled smoke test, but that should be treated as
a shortcut, not the default desktop design.

`pipewire-pulse` is PipeWire's PulseAudio-compatible server implementation. It
is not built from the PulseAudio daemon codebase. Pulse clients still use the
normal PulseAudio client libraries and tools, but the server side is PipeWire's
`libpipewire-module-protocol-pulse`.

ALSA and JACK compatibility normally do not add standing daemons. They are
client-side libraries/config that connect apps to PipeWire.

The Android bridge should initially live in the existing app/compositor process
as native/Kotlin-managed threads. Splitting it into a separate Android-side
helper process can be revisited only if isolation or lifecycle pressure makes
that valuable.

## Endpoint Ownership

The tawc app should own endpoint creation and cleanup.

Candidate endpoint names:

```text
/usr/share/tawc/audio-out-0
/usr/share/tawc/audio-in-0
```

Those names are the rootfs-visible paths. The backing paths should be under the
app-private shared directory that install methods already bind to
`/usr/share/tawc/`.

Open questions for implementation:

- FIFO vs Unix socket vs regular pipe exposed through a helper. PipeWire's
  pipe-tunnel module is designed around FIFO/pipe-style raw PCM; start with
  FIFOs unless testing shows bad blocking behavior.
- Which side creates the FIFO. Prefer tawc creating it so stale endpoints and
  permissions are under app control.
- Open ordering. FIFOs can block on open until the other side appears. The
  bridge should handle this deliberately instead of relying on incidental daemon
  startup order.
- Backpressure. The Android output clock is authoritative. The bridge needs a
  bounded buffer and clear behavior for underruns/overruns.
- Cleanup. Remove stale audio endpoints when the compositor/session stops, just
  like other app-owned runtime files.

## PipeWire Configuration Sketch

Exact keys should be verified against the PipeWire version installed in the
target distro. The intended shape is:

```ini
context.modules = [
  {
    name = libpipewire-module-pipe-tunnel
    args = {
      tunnel.mode = sink
      tunnel.may-pause = false
      pipe.filename = "/usr/share/tawc/audio-out-0"
      audio.format = "S16LE"
      audio.rate = 48000
      audio.channels = 2
      node.name = "tawc_output"
      node.description = "tawc Android output"
      stream.props = {
        media.class = "Audio/Sink"
      }
    }
  }
  {
    name = libpipewire-module-pipe-tunnel
    args = {
      tunnel.mode = source
      tunnel.may-pause = false
      pipe.filename = "/usr/share/tawc/audio-in-0"
      audio.format = "S16LE"
      audio.rate = 48000
      audio.channels = 1
      node.name = "tawc_input"
      node.description = "tawc Android microphone"
      stream.props = {
        media.class = "Audio/Source"
      }
    }
  }
]
```

The PipeWire docs state that the module defaults to 16-bit stereo 48 kHz when
format is not specified, but tawc should specify it anyway so the Android bridge
and logs have a stable contract.

`pipewire-pulse` should listen on an in-rootfs Unix socket under
`$XDG_RUNTIME_DIR/pulse/native` or another explicit path. `RootfsEnv` currently
sets `XDG_RUNTIME_DIR=/tmp`, so the first implementation can use `/tmp` unless
we introduce a more private per-install runtime directory.

Rootfs env additions will likely include:

```text
PIPEWIRE_RUNTIME_DIR=/tmp
PULSE_SERVER=unix:/tmp/pulse/native
```

ALSA compatibility should be configured through the distro's PipeWire ALSA
plugin, not through a fake ALSA device in tawc.

## Android Bridge

Playback bridge:

- open/read `/usr/share/tawc/audio-out-0` backing endpoint.
- feed PCM into `AudioTrack` or AAudio.
- start with a conservative buffer size; expose logs for underruns.
- honor Android audio focus and output route changes.
- stop cleanly when no session is active.

Capture bridge:

- require Android `RECORD_AUDIO`.
- default off until explicitly enabled.
- create/use `AudioRecord` or AAudio input only while capture is enabled.
- write fixed-format PCM to `/usr/share/tawc/audio-in-0`.
- surface permission denial and mic-disabled states clearly.

AAudio/Oboe is attractive for lower latency and native code integration, but
`AudioTrack`/`AudioRecord` may be faster to prototype from Kotlin. The bridge
contract should not depend on which Android API is used internally.

## First Milestones

1. Playback-only smoke:
   - install PipeWire, WirePlumber, `pipewire-pulse`, and ALSA PipeWire
     integration in the rootfs.
   - create `/usr/share/tawc/audio-out-0`.
   - load a PipeWire pipe-tunnel sink.
   - play `paplay`, `pw-play`, and an app-level sound through Android output.

2. Compatibility:
   - make Pulse clients use `pipewire-pulse`.
   - make ALSA clients route to PipeWire.
   - verify Firefox or another real desktop app.

3. Lifecycle:
   - start/stop the audio stack with the tawc session or install runtime.
   - make repeated sessions not leave stale FIFOs/daemons.
   - log underruns, daemon failures, and missing modules.

4. Capture:
   - add `/usr/share/tawc/audio-in-0`.
   - gate with `RECORD_AUDIO` permission and a user-visible setting.
   - verify `pw-record` and Pulse recording clients.

5. Tuning:
   - reduce latency after correctness is stable.
   - decide whether the Android bridge should use AAudio/Oboe.
   - decide whether WirePlumber policy needs a tawc-specific profile/rule.

## Pure PulseAudio Option

The simpler non-PipeWire alternative is:

```text
PulseAudio clients
  -> pulseaudio daemon
  -> PulseAudio pipe sink/source or tawc-specific Pulse module
  -> /usr/share/tawc/audio-{out,in}
  -> tawc Android bridge
```

ALSA-only clients can route into PulseAudio through `alsa-plugins`.

This option has fewer moving parts and may be easier to debug:

- one PulseAudio server process instead of PipeWire + WirePlumber +
  `pipewire-pulse`.
- PulseAudio already owns client mixing, resampling, stream volume, and the
  Pulse protocol.
- a pipe sink/source is conceptually close to the Android bridge contract.

The downside is strategic: it does not cover native PipeWire clients unless they
fall back to PulseAudio/ALSA, and it invests in the older desktop audio stack.
It remains a fallback if PipeWire's process count, configuration, or pipe-tunnel
behavior becomes a blocker.

## Options We Are Not Starting With

### Custom PulseAudio Server In tawc

tawc could implement the server side of the PulseAudio native protocol and map
client streams to Android `AudioTrack`s, letting Android mix them.

Lean away from this. A useful Pulse server is not just PCM over a socket. It
must handle authentication/cookies, stream creation, timing queries, latency,
drain/flush/cork, device enumeration, per-stream volume, subscriptions,
recording streams, monitor sources, and client quirks. Reimplementing enough of
that for browsers/toolkits is likely more work than running `pipewire-pulse` or
PulseAudio.

### Embed PipeWire Server In The Android App

PipeWire is library/module based, so embedding a server in the Android app is
theoretically possible.

Lean away from this. It would require building PipeWire, SPA plugins, and
modules for Android/Bionic, packaging their config/module paths inside the APK,
hosting Unix sockets reachable from rootfs processes, and probably still
running or embedding `pipewire-pulse` and policy logic. It also couples audio
server crashes/deadlocks directly to the compositor/app process.

Using `libpipewire` in the app as a client is different and may be useful later,
but it does not remove the need for the rootfs PipeWire server.

### Fake ALSA Device

Lean away from this. Android apps do not normally get useful direct access to
Linux ALSA devices, and Linux desktop audio behavior is not just an ALSA PCM.
Use PipeWire/PulseAudio to satisfy desktop clients, then bridge one controlled
PCM edge to Android.

### Pulse Tunnel From PipeWire To PulseAudio

PipeWire has `libpipewire-module-pulse-tunnel`, which can expose a PipeWire
source/sink backed by a PulseAudio server.

This is not needed for the preferred plan. `pipe-tunnel` lets PipeWire connect
directly to tawc's PCM endpoint, and `pipewire-pulse` handles PulseAudio clients
on top of PipeWire.

### Network PulseAudio

PulseAudio can listen over TCP, and PipeWire's Pulse server can also expose TCP
sockets. Avoid this for tawc's local bridge. Unix-domain endpoints under the
app-private shared namespace are easier to permission, clean up, and keep off
the network.

## Open Risks

- Android may kill rootfs daemons under memory/lifecycle pressure. This is a
  lower priority than proving audio, but the stack should be restartable.
- FIFO blocking semantics may be awkward across tawc session start/stop. Test
  early with daemon restarts and missing reader/writer cases.
- PipeWire's pipe tunnel may not provide enough latency/clock feedback for good
  video sync or low-latency audio. If this becomes the limit, a real PipeWire
  Android-backed node may be the long-term solution.
- Mic capture has privacy and permission requirements. Do not silently start
  recording.
- Bluetooth/headset route changes and Android audio focus should be treated as
  first-class lifecycle events before calling audio production-ready.

