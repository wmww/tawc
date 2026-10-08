# Curated license texts

License texts for shipped components whose license lives only in package
metadata (a Maven POM or a project website), so it cannot be read out of
any `deps/` checkout. `scripts/gen-third-party-licenses.sh` folds these
into the generated in-app attribution file.

Everything else gets its text straight from the vendored source tree or
the local cargo registry — add a file here only when there is genuinely
no in-tree source for it.

| File | Component | Origin |
|---|---|---|
| `bouncycastle.txt` | `org.bouncycastle:*` | <https://www.bouncycastle.org/about/license/> |
| `xz-java.txt` | `org.tukaani:xz` | <https://github.com/tukaani-project/xz-java> `COPYING` |
| `zstd-jni.txt` | `com.github.luben:zstd-jni` | <https://github.com/luben/zstd-jni> `LICENSE` |
| `hack.txt` | Hack v3.003 terminal font (`app/src/main/res/font/hack_regular.ttf`) | <https://github.com/source-foundry/Hack> `LICENSE.md` |
| `rfc1751.txt` | RFC 1751 dictionary in `remote/src/sid/` | <https://www.rfc-editor.org/rfc/rfc1751> |
