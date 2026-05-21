# Kotlin UI polish and low-risk lint cleanup

The Android UI is mostly built imperatively in Kotlin. That has kept
iteration fast, but lint now reports a lot of noise that makes real Android
API warnings harder to spot.

Scope this as a behavior-preserving cleanup pass, not a UI redesign.

## Hardcoded UI strings

Many visible labels/messages are assigned directly in Kotlin
(`TextView.text = "..."`, `primaryButton("Install")`, dynamic messages built
with string templates). Move stable, user-facing strings into
`res/values/strings.xml`.

Keep this pragmatic:

- Move Activity titles, button labels, empty states, settings labels, and
  dialog copy.
- Use string resources with placeholders for dynamic visible text.
- Leave log lines, broker/dev protocol errors, and low-level diagnostic
  exceptions in code unless they are primary user-facing UI.
- Do not rewrite layouts or copy beyond what is needed for resource strings.

## KTX suggestions

Apply the current lint KTX suggestions:

- `Uri.parse(...)` -> `String.toUri()`
- `SharedPreferences.edit().put*().apply()` -> `SharedPreferences.edit { ... }`
- `ColorDrawable(Color.TRANSPARENT)` -> `Color.TRANSPARENT.toDrawable()`

Add imports from AndroidX KTX/Core as needed, but do not introduce new
architectural dependencies.

## Other low-risk Kotlin cleanup

While touching the same files, fix similarly mechanical Kotlin lint/readability
items only when they do not change behavior or ownership boundaries:

- Replace repeated tiny boilerplate with existing local helpers where obvious.
- Prefer typed AndroidX compat/KTX helpers over verbose platform calls when the
  app already depends on them.
- Keep comments short and only where they explain a real platform quirk.

Out of scope:

- Changing navigation, Activity lifecycle, install behavior, or rootfs paths.
- Broad UI redesign.
- Dependency upgrades beyond what is already present transitively.
- Large refactors of the imperative UI construction style.

