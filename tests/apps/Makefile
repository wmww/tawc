REPO := $(abspath $(CURDIR)/../..)

ABI ?= aarch64
DISTRO ?= arch
SYSROOT := $(REPO)/build/sysroots/$(DISTRO)-$(ABI)
OUT_ROOT := $(REPO)/build/test-apps/$(DISTRO)-$(ABI)

ifeq ($(ABI),aarch64)
ifeq ($(origin CC),default)
CC := $(or $(TAWC_AARCH64_CC),aarch64-linux-gnu-gcc)
endif
NDK_TRIPLE := aarch64-linux-android
HYBRIS_APPS := tawc-dri-test libhybris-tls-repro
else ifeq ($(ABI),x86_64)
ifeq ($(origin CC),default)
CC := $(or $(TAWC_X86_64_CC),$(shell command -v x86_64-linux-gnu-gcc >/dev/null 2>&1 && echo x86_64-linux-gnu-gcc || echo gcc))
endif
NDK_TRIPLE := x86_64-linux-android
HYBRIS_APPS :=
else
$(error Unsupported ABI '$(ABI)')
endif

PKG_CONFIG_ENV := PKG_CONFIG_SYSROOT_DIR="$(SYSROOT)" PKG_CONFIG_LIBDIR="$(SYSROOT)/usr/lib/pkgconfig:$(SYSROOT)/usr/share/pkgconfig" PKG_CONFIG_PATH=
PC := $(PKG_CONFIG_ENV) pkg-config

WAYLAND_PROTOCOLS_DIR := $(SYSROOT)/usr/share/wayland-protocols
XDG_SHELL_XML := $(WAYLAND_PROTOCOLS_DIR)/stable/xdg-shell/xdg-shell.xml
TEXT_INPUT_XML := $(WAYLAND_PROTOCOLS_DIR)/unstable/text-input/text-input-unstable-v3.xml
FRACTIONAL_SCALE_XML := $(WAYLAND_PROTOCOLS_DIR)/staging/fractional-scale/fractional-scale-v1.xml
XDG_DECORATION_XML := $(WAYLAND_PROTOCOLS_DIR)/unstable/xdg-decoration/xdg-decoration-unstable-v1.xml
XDG_ACTIVATION_XML := $(WAYLAND_PROTOCOLS_DIR)/staging/xdg-activation/xdg-activation-v1.xml

CFLAGS_SYSROOT := --sysroot=$(SYSROOT) -B$(SYSROOT)/usr/lib -L$(SYSROOT)/usr/lib -L$(SYSROOT)/lib -O2 -g
LDFLAGS_SYSROOT := --sysroot=$(SYSROOT) -Wl,-rpath-link,$(SYSROOT)/usr/lib -Wl,-rpath-link,$(SYSROOT)/usr/lib/hybris
HYBRIS_COMMON := $(SYSROOT)/usr/lib/hybris/libhybris-common.so

ANDROID_HOME ?= $(HOME)/Android/Sdk
NDK_ROOT ?= $(or $(ANDROID_NDK_ROOT),$(ANDROID_NDK_HOME),$(ANDROID_HOME)/ndk)
NDK_BIN_DIR := $(shell if [ -x "$(NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin/$(NDK_TRIPLE)29-clang" ]; then echo "$(NDK_ROOT)/toolchains/llvm/prebuilt/linux-x86_64/bin"; else ls -d "$(NDK_ROOT)"/*/toolchains/llvm/prebuilt/linux-x86_64/bin 2>/dev/null | sort -V | tail -n1; fi)
NDK_CLANG := $(NDK_BIN_DIR)/$(NDK_TRIPLE)29-clang

BASE_APPS := wayland-debug-app x11-debug-app eglx11-test
APPS := $(BASE_APPS) $(HYBRIS_APPS)

.PHONY: all clean sysroot $(APPS) adreno-struct-varying

all: $(APPS)

wayland-debug-app: $(OUT_ROOT)/wayland-debug-app/wayland-debug-app
x11-debug-app: $(OUT_ROOT)/x11-debug-app/x11-debug-app
eglx11-test: $(OUT_ROOT)/eglx11-test/eglx11-test
tawc-dri-test: $(OUT_ROOT)/tawc-dri-test/tawc-dri-test
libhybris-tls-repro: $(OUT_ROOT)/libhybris-tls-repro/libhybris-tls-repro $(OUT_ROOT)/libhybris-tls-repro/tls_lib.so $(OUT_ROOT)/libhybris-tls-repro/weak_lib.so
adreno-struct-varying: $(OUT_ROOT)/adreno-struct-varying/adreno-struct-varying

$(OUT_ROOT)/%/:
	mkdir -p "$@"

sysroot:
	@if [ ! -d "$(SYSROOT)/usr" ] || ! grep -q " full " "$(SYSROOT)/.tawc-sysroot" 2>/dev/null; then \
		"$(REPO)/scripts/build-host-sysroot.sh" "--abi=$(ABI)" "--distro=$(DISTRO)" --profile=full; \
	fi

$(XDG_SHELL_XML) $(TEXT_INPUT_XML) $(FRACTIONAL_SCALE_XML) $(XDG_DECORATION_XML) $(XDG_ACTIVATION_XML): | sysroot
	@test -f "$@" || { echo "ERROR: missing $@ after sysroot setup" >&2; exit 1; }

$(OUT_ROOT)/wayland-debug-app/xdg-shell-client-protocol.h: $(XDG_SHELL_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner client-header "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/xdg-shell-protocol.c: $(XDG_SHELL_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner private-code "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/text-input-unstable-v3-client-protocol.h: $(TEXT_INPUT_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner client-header "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/text-input-unstable-v3-protocol.c: $(TEXT_INPUT_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner private-code "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/fractional-scale-v1-client-protocol.h: $(FRACTIONAL_SCALE_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner client-header "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/fractional-scale-v1-protocol.c: $(FRACTIONAL_SCALE_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner private-code "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/xdg-decoration-unstable-v1-client-protocol.h: $(XDG_DECORATION_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner client-header "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/xdg-decoration-unstable-v1-protocol.c: $(XDG_DECORATION_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner private-code "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/xdg-activation-v1-client-protocol.h: $(XDG_ACTIVATION_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner client-header "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/xdg-activation-v1-protocol.c: $(XDG_ACTIVATION_XML) | sysroot $(OUT_ROOT)/wayland-debug-app/
	wayland-scanner private-code "$<" "$@"

$(OUT_ROOT)/wayland-debug-app/wayland-debug-app: wayland-debug-app/wayland-debug-app.c \
		$(OUT_ROOT)/wayland-debug-app/xdg-shell-client-protocol.h \
		$(OUT_ROOT)/wayland-debug-app/xdg-shell-protocol.c \
		$(OUT_ROOT)/wayland-debug-app/text-input-unstable-v3-client-protocol.h \
		$(OUT_ROOT)/wayland-debug-app/text-input-unstable-v3-protocol.c \
		$(OUT_ROOT)/wayland-debug-app/fractional-scale-v1-client-protocol.h \
		$(OUT_ROOT)/wayland-debug-app/fractional-scale-v1-protocol.c \
		$(OUT_ROOT)/wayland-debug-app/xdg-decoration-unstable-v1-client-protocol.h \
		$(OUT_ROOT)/wayland-debug-app/xdg-decoration-unstable-v1-protocol.c \
		$(OUT_ROOT)/wayland-debug-app/xdg-activation-v1-client-protocol.h \
		$(OUT_ROOT)/wayland-debug-app/xdg-activation-v1-protocol.c
	$(CC) $(CFLAGS_SYSROOT) -I"$(OUT_ROOT)/wayland-debug-app" -o "$@" \
		wayland-debug-app/wayland-debug-app.c \
		$(OUT_ROOT)/wayland-debug-app/xdg-shell-protocol.c \
		$(OUT_ROOT)/wayland-debug-app/text-input-unstable-v3-protocol.c \
		$(OUT_ROOT)/wayland-debug-app/fractional-scale-v1-protocol.c \
		$(OUT_ROOT)/wayland-debug-app/xdg-decoration-unstable-v1-protocol.c \
		$(OUT_ROOT)/wayland-debug-app/xdg-activation-v1-protocol.c \
		$$($(PC) --cflags wayland-client cairo) $$($(PC) --libs wayland-client cairo) \
		$(LDFLAGS_SYSROOT) -std=c11 -Wall -Wextra -Werror

$(OUT_ROOT)/eglx11-test/eglx11-test: eglx11-test/eglx11-test.c | sysroot $(OUT_ROOT)/eglx11-test/
	$(CC) $(CFLAGS_SYSROOT) -o "$@" "$<" \
		$$($(PC) --cflags x11) $$($(PC) --libs x11) \
		-lEGL -lGLESv2 -L"$(SYSROOT)/usr/lib/hybris" -Wl,-rpath,/usr/lib/hybris \
		-ldl $(LDFLAGS_SYSROOT) -Wall -Wextra

$(OUT_ROOT)/x11-debug-app/x11-debug-app: x11-debug-app/x11-debug-app.c | sysroot $(OUT_ROOT)/x11-debug-app/
	$(CC) $(CFLAGS_SYSROOT) -o "$@" "$<" \
		$$($(PC) --cflags x11) $$($(PC) --libs x11) \
		$(LDFLAGS_SYSROOT) -Wall -Wextra -Werror

$(HYBRIS_COMMON): | sysroot
	"$(REPO)/scripts/build-libhybris.sh"
	mkdir -p "$(SYSROOT)/usr/lib/hybris"
	cp -a "$(REPO)/build/libhybris-aarch64/install/usr/lib/hybris/." "$(SYSROOT)/usr/lib/hybris/"

$(OUT_ROOT)/tawc-dri-test/tawc-dri-test: tawc-dri-test/tawc-dri-test.c $(HYBRIS_COMMON) | sysroot $(OUT_ROOT)/tawc-dri-test/
	$(CC) $(CFLAGS_SYSROOT) -o "$@" "$<" \
		$$($(PC) --cflags xcb) $$($(PC) --libs xcb) \
		-L"$(SYSROOT)/usr/lib/hybris" -Wl,-rpath,/usr/lib/hybris -lhybris-common \
		-ldl $(LDFLAGS_SYSROOT) -Wall -Wextra

$(OUT_ROOT)/libhybris-tls-repro/libhybris-tls-repro: libhybris-tls-repro/repro.c $(HYBRIS_COMMON) | sysroot $(OUT_ROOT)/libhybris-tls-repro/
	$(CC) $(CFLAGS_SYSROOT) -O0 -g -o "$@" "$<" \
		-L"$(SYSROOT)/usr/lib/hybris" -lhybris-common -Wl,-rpath,/usr/lib/hybris \
		-pthread $(LDFLAGS_SYSROOT) -Wall -Wextra

$(OUT_ROOT)/libhybris-tls-repro/tls_lib.so: libhybris-tls-repro/tls_lib.c | sysroot $(OUT_ROOT)/libhybris-tls-repro/
	test -x "$(NDK_CLANG)" || { echo "ERROR: cannot locate Android NDK clang under $(NDK_ROOT)" >&2; exit 1; }
	"$(NDK_CLANG)" -fPIC -shared -o "$@" "$<"

$(OUT_ROOT)/libhybris-tls-repro/weak_lib.so: libhybris-tls-repro/weak_lib.c | sysroot $(OUT_ROOT)/libhybris-tls-repro/
	test -x "$(NDK_CLANG)" || { echo "ERROR: cannot locate Android NDK clang under $(NDK_ROOT)" >&2; exit 1; }
	"$(NDK_CLANG)" -fPIC -shared -o "$@" "$<"

$(OUT_ROOT)/adreno-struct-varying/xdg-shell-client-protocol.h: $(XDG_SHELL_XML) | sysroot $(OUT_ROOT)/adreno-struct-varying/
	wayland-scanner client-header "$<" "$@"

$(OUT_ROOT)/adreno-struct-varying/xdg-shell-protocol.c: $(XDG_SHELL_XML) | sysroot $(OUT_ROOT)/adreno-struct-varying/
	wayland-scanner private-code "$<" "$@"

$(OUT_ROOT)/adreno-struct-varying/adreno-struct-varying: adreno-struct-varying/adreno-struct-varying.c \
		$(OUT_ROOT)/adreno-struct-varying/xdg-shell-client-protocol.h \
		$(OUT_ROOT)/adreno-struct-varying/xdg-shell-protocol.c
	$(CC) $(CFLAGS_SYSROOT) -I"$(OUT_ROOT)/adreno-struct-varying" -Wl,--no-as-needed -o "$@" \
		adreno-struct-varying/adreno-struct-varying.c \
		$(OUT_ROOT)/adreno-struct-varying/xdg-shell-protocol.c \
		$$($(PC) --cflags wayland-client wayland-egl) \
		-L"$(SYSROOT)/usr/lib/hybris" -Wl,-rpath,/usr/lib/hybris \
		-lwayland-egl -lwayland-client -lEGL -lGLESv2 -ldl \
		$(LDFLAGS_SYSROOT) -Wall -Wextra

clean:
	rm -rf "$(OUT_ROOT)"
