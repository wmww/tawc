// Tells Firefox to source extra prefs from /usr/lib/firefox/firefox.cfg.
// This file lives at /usr/lib/firefox/defaults/pref/autoconfig.js and
// is read for every profile on every startup, so it has to ship with
// the distro Firefox install (not in the user profile).
//
// `obscure_value=0` disables the Mozilla autoconfig "byte-rotate the
// .cfg by N bytes" obfuscation — we ship the .cfg as plain JS, so 0
// is the only sensible value here.
pref("general.config.filename", "firefox.cfg");
pref("general.config.obscure_value", 0);
