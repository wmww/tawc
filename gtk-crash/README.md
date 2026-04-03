# GTK3 + libhybris TLS crash reproducer

See [issues/gtk-libhybris-tls-crash.md](../issues/gtk-libhybris-tls-crash.md) for full details.

This directory contains `repro.c` and `build-and-test.sh` for reproducing the intermittent `glGenTextures` SIGSEGV when GTK3 is loaded via libhybris.
