# Emulator: compositor cycles leak goldfish pipe fds

`lazy_compositor::test_compositor_cycles_do_not_leak` fails on the x86_64
emulator (gfxstream default backend), every run:

```
fds grew over 8 compositor cycles: 137 -> 151 (/dev/goldfish_pipe_dprctd: 13 -> 29)
```

Two goldfish pipe fds per compositor start/stop, so likely an EGL/GL
context or gralloc connection the emulator's GL stack opens per cycle and
`compositor: destroy the EGL context on stop` (4f13c1f) doesn't release.
Seen at 1b8528d with and without unrelated tawcroot changes; not yet
checked on a physical device.
