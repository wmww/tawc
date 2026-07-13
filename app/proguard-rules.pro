# NOTE: minification is deliberately OFF for all build types (see the
# release block in build.gradle.kts), so this file is currently inert.
# The rules are kept correct anyway so that enabling R8 stays a
# one-flag change, verified 2026-07: with these rules, a minified build
# passed terminal + compositor smoke tests on the emulator.

# NativeBridge is looked up from Rust by string name (find_class) and
# its `external fun` + `@JvmStatic` callback methods are resolved by
# JNI symbol mangling / call_static_method by name. Keep the class name
# and all members verbatim.
-keep class me.phie.tawc.compositor.NativeBridge { *; }

# Keep file/line info so crash traces can be decoded with the build's
# mapping.txt. Without LineNumberTable, retrace can't resolve line
# numbers or disambiguate inlined frames.
-keepattributes SourceFile,LineNumberTable
-renamesourcefileattribute SourceFile

# zstd-jni's native library resolves Java fields and methods by name
# (GetFieldID/GetMethodID on ZstdInputStreamNoFinalizer and friends) and
# the AAR ships no consumer rules. Keep its whole (tiny) Java surface.
-keep class com.github.luben.zstd.** { *; }
