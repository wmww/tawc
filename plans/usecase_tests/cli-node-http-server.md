# Usecase test: Node.js and a local HTTP server

Read [README.md](README.md) first for shared procedure, cleanup, and
reporting rules.

**Target:** emulator or physical.
**Usecase:** a JS developer installs node, npm-installs a package, and runs a local web server.

## Prerequisites

- Cache proxy up (README step 6) for pacman.

## Steps

Work under `/root/usecase-node/`.

1. `pacman -S --noconfirm nodejs npm`.
2. `node -e 'console.log(21*2)'` sanity.
3. `npm init -y && npm install ms` (tiny package; npm registry is direct
   network, not proxied). `node -e 'console.log(require("ms")("2 days"))'`.
4. Write a plain `http` server listening on `127.0.0.1:3000` that serves
   a fixed body; start it in the background; `curl -s http://127.0.0.1:3000/`
   from a second `rootfs-run.sh` session and verify the body; then a
   loop of ~50 requests to shake out fd/socket handling; stop the server.
5. Also bind on `0.0.0.0:3000` and confirm it still works via
   `127.0.0.1` (users will copy configs that bind all interfaces).

## Expected results

- Install, npm, and the server all work; every curl returns the expected
  body; server shuts down cleanly.

## Known issues / caveats

- Node/libuv may probe `io_uring`; tawcroot returns ENOSYS and libuv
  falls back to epoll — expected, not a bug (notes/tawcroot/status.md).
- Ports below 1024 will fail (no real CAP_NET_BIND_SERVICE) — out of
  scope here; covered conceptually in cli-ssh-server.

## Cleanup

Kill the server, remove `/root/usecase-node/`,
`pacman -Rns nodejs npm`.
