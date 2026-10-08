# cache proxy abandoned fills can block fresh installs long enough to timeout

During an Arch Linux ARM install on a physical device, the bootstrap
download failed twice with:

    java.net.SocketTimeoutException: timeout
    at me.phie.tawc.install.Downloader.download(Downloader.kt:73)

The app logged the proxied tarball URL, but nginx's access log only
showed the bootstrap `HEAD`; the matching `GET` never completed before
the app timed out. At the same time `build/cache-proxy/tmp/proxy/`
contained several large temp cache files still growing from previous
proxy clients. After those temp fills drained, uninstalling the failed
slot and retrying the same install immediately reached `VERIFYING` and
then completed.

## Repro Shape

1. Start the cache proxy.
2. Have another proxied install/test-deps run fill several large cache
   entries.
3. Start a fresh proxied bootstrap install:

       scripts/tawc-exec.sh --foreground-app --action install \
           --arg id=arch --arg distro=arch \
           --arg mirrorProxy=http://127.0.0.1:8080/proxy/

4. Watch `build/cache-proxy/tmp/proxy/` and nginx access logs. The app
   can time out waiting for response headers while nginx is still
   writing large temp files.

Seen again 2026-08-10 on the emulator with a Void bootstrap: the app
sat 300 s (Downloader's `readTimeout`) in
`Http1xStream.readResponseHeaders` before throwing, while
`tmp/proxy/` held several growing temp files. A host `curl` for the
same URL a few minutes later got response headers in 0.8 s and the
retried install went through. `proxy_cache_lock on` in
`scripts/cache-proxy.conf` is a suspect worth checking.

## Possible Fixes

- Configure nginx so it stops cache fills promptly after the client
  disconnects, if stock proxy-cache behavior is keeping abandoned fills
  alive.
- Add a cache-proxy status command that surfaces active temp fills and
  their ages before install/test scripts start.
- Consider separate cache zones or request limits for bootstrap
  tarballs versus package downloads so a package flood cannot block the
  next bootstrap.

## Severity

Medium for dev loops. The install recovers cleanly after uninstall and
retry, but the failure mode looks like a distro/network bug until the
proxy temp directory is inspected.
