# Jaffedrone — 5-string drone instrument for the TrimUI Brick, Brick Hammer and Brick Pro, running
# MustardOS
#
#   make          cross-compile both builds with zig (aarch64, glibc 2.28, against the device's SDL2):
#                   build/brick/jaffedrone      TrimUI Brick and Brick Hammer
#                   build/brickpro/jaffedrone   TrimUI Brick Pro (adds the analog sticks)
#   make package  both, as MustardOS application archives:
#                   build/Jaffedrone-mustardos-brick.muxapp, build/Jaffedrone-mustardos-brickpro.muxapp
#   make test     build the host harness and render screenshots + audio into build/test
#   make install SD=/path/to/card [DEVICE=brickpro]
#                 copy the app into SD/MUOS/application/Jaffedrone
#   make update-test   install a fake release over a test app folder, the way the app updates itself
#
# zig: ZIG=/path/to/zig, or `pip install ziglang==0.14.1` (used as `python3 -m ziglang`)

ZIG    ?= $(shell command -v zig 2>/dev/null || echo python3 -m ziglang)
TARGET := aarch64-linux-gnu.2.28
CPU    := cortex_a53
SDL_INC := third_party/SDL2-2.30.9-headers

SRC    := src/engine.c src/ui.c src/gfx.c src/sync.c src/aclock.c src/log.c
HDR    := src/engine.h src/ui.h src/gfx.h src/sync.h src/aclock.h src/log.h src/font_data.h src/logo_data.h
NET    := src/net.c src/update.c # the self-updater, on the device and in build/jd_update
PLAT   := src/ahead.c src/midi.c src/link.c src/sticks.c src/perflog.c src/main_trimui.c
PLATH  := src/ahead.h src/midi.h src/link.h src/sticks.h src/perflog.h src/net.h src/update.h
CFLAGS := -O2 -std=gnu11 -Wall -Wextra -fno-math-errno
APP    := dist/Jaffedrone

# the app's version: a release passes its tag's (`make package JD_VERSION=0.8.0`), otherwise the one in
# src/engine.h. build/version changes only when it does, so the builds and the manual follow it.
JD_VERSION ?=
ifneq ($(JD_VERSION),)
CFLAGS += -DJD_VERSION='"$(JD_VERSION)"'
endif
HDR    += build/version

all: build/brick/jaffedrone build/brickpro/jaffedrone $(APP)/splash.jdi $(APP)/LICENSE-mbedtls.txt

src/logo_data.h: tools/mklogo.py
	python3 tools/mklogo.py $@

src/font_data.h: tools/mkfont.py
	python3 tools/mkfont.py /usr/share/fonts/google-noto/NotoSansMono-Bold.ttf $@

$(APP)/splash.jdi: tools/mksplash.py art/roofline.jpg src/logo_data.h src/ui.c
	python3 tools/mksplash.py art/roofline.jpg $@

# a stand-in for the device's libSDL2-2.0.so.0, to link against (the device has the real one)
build/sdl2/libSDL2-2.0.so.0: tools/mksdlstub.py src/main_trimui.c
	@mkdir -p build/sdl2
	python3 tools/mksdlstub.py build/sdl2/sdl2_stub.c src/main_trimui.c
	$(ZIG) cc -target $(TARGET) -shared -Wl,-soname,libSDL2-2.0.so.0 -o $@ build/sdl2/sdl2_stub.c

# Mbed TLS (Apache-2.0) for the updater's HTTPS, from a pinned release, not committed; each
# toolchain builds its own libmbedtls.a from it with tools/mbedtls.mk (as in Columbia Street)
MBED_VER := 3.6.7
MBED_SHA := a7e8bcbec0e6f761b4af24f25677626b35f762f68eef79c08677a363212d11f6
MBED_DIR := third_party/mbedtls
MBED_URL := https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$(MBED_VER)/mbedtls-$(MBED_VER).tar.bz2
MBED_SRC := $(MBED_DIR)/.v$(MBED_VER)
$(MBED_SRC):
	rm -rf $(MBED_DIR) && mkdir -p $(MBED_DIR)
	curl -sSL -o $(MBED_DIR).tar.bz2 $(MBED_URL) || wget -qO $(MBED_DIR).tar.bz2 $(MBED_URL)
	echo "$(MBED_SHA)  $(MBED_DIR).tar.bz2" | sha256sum -c --quiet || (rm -f $(MBED_DIR).tar.bz2; false)
	tar -xjf $(MBED_DIR).tar.bz2 -C $(MBED_DIR) --strip-components=1
	rm $(MBED_DIR).tar.bz2
	touch $@

# the binary carries Mbed TLS, so the app folder carries its licence
$(APP)/LICENSE-mbedtls.txt: $(MBED_SRC)
	cp $(MBED_DIR)/LICENSE $@

build/zig/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/zig MBED_DIR=$(MBED_DIR) \
		CC="$(ZIG) cc -target $(TARGET) -mcpu=$(CPU)" AR="$(ZIG) ar" MBED_CFLAGS=-O2

build/host/libmbedtls.a: $(MBED_SRC)
	$(MAKE) -f tools/mbedtls.mk OUT=build/host MBED_DIR=$(MBED_DIR) CC=cc AR=ar MBED_CFLAGS=-O2

DEVICE_CC = $(ZIG) cc -target $(TARGET) -mcpu=$(CPU) $(CFLAGS) -I$(SDL_INC) -I$(MBED_DIR)/include -Isrc
DEVICE_LINK = build/zig/libmbedtls.a build/sdl2/libSDL2-2.0.so.0 -lm -lpthread -Wl,--allow-shlib-undefined -s

build/brick/jaffedrone: $(SRC) $(NET) $(PLAT) $(HDR) $(PLATH) build/sdl2/libSDL2-2.0.so.0 build/zig/libmbedtls.a
	@mkdir -p build/brick
	$(DEVICE_CC) -o $@ $(SRC) $(NET) $(PLAT) $(DEVICE_LINK)

build/brickpro/jaffedrone: $(SRC) $(NET) $(PLAT) $(HDR) $(PLATH) build/sdl2/libSDL2-2.0.so.0 build/zig/libmbedtls.a
	@mkdir -p build/brickpro
	$(DEVICE_CC) -DJD_BRICKPRO -o $@ $(SRC) $(NET) $(PLAT) $(DEVICE_LINK)

# the host updater: what the app does on A, from the command line (src/main_update.c)
build/jd_update: $(NET) src/main_update.c src/engine.h src/net.h src/update.h build/version build/host/libmbedtls.a
	cc $(CFLAGS) -I$(MBED_DIR)/include -o $@ $(NET) src/main_update.c build/host/libmbedtls.a -lpthread

update-test: build/jd_update
	sh tools/update_test.sh

build/jd_test: $(SRC) src/ahead.c src/midi.c src/link.c src/sticks.c src/perflog.c src/main_test.c $(HDR) $(PLATH)
	@mkdir -p build
	cc $(CFLAGS) -o $@ $(SRC) src/ahead.c src/midi.c src/link.c src/sticks.c src/perflog.c src/main_test.c -lm -lpthread

build/jd_bench: $(SRC) src/main_bench.c $(HDR)
	@mkdir -p build
	cc $(CFLAGS) -o $@ $(SRC) src/main_bench.c -lm

# the load of many voice mixtures at their worst, heaviest first
bench: build/jd_bench
	build/jd_bench

test: build/jd_test
	@mkdir -p build/test
	build/jd_test build/test
	cd build/test && for f in *.ppm; do magick $$f $${f%.ppm}.png && rm $$f; done

# MustardOS: a .muxapp is a zip that its Archive Manager unpacks into the application folder
# (MUOS/application on the card), and the folder's name is the name in the Applications list.
# The app folder, binary and manual, plus mux_launch.sh (how MustardOS starts an app),
# wake_screen.sh (the Brick Pro's screen can stay dark otherwise) and the list glyph.
MUX := dist/mustardos
MUX_APP := Jaffedrone
build/Jaffedrone-mustardos-%.muxapp: build/%/jaffedrone $(APP)/splash.jdi $(APP)/LICENSE-mbedtls.txt docs/Jaffedrone-Manual.pdf \
		$(MUX)/mux_launch.sh $(MUX)/wake_screen.sh $(MUX)/glyph/jaffedrone.svg
	rm -rf build/mux-$* $@
	mkdir -p "build/mux-$*/$(MUX_APP)"
	cp -r $(APP)/. "build/mux-$*/$(MUX_APP)/"
	cp -r $(MUX)/. "build/mux-$*/$(MUX_APP)/"
	cd "build/mux-$*/$(MUX_APP)" && rm -f *.sav* *.jds log.txt jaffedrone_log.txt* jaffedrone.cfg \
		jaffedrone_ui.cfg jaffedrone_stick.cfg midi.cfg sysinfo.txt
	cp build/$*/jaffedrone docs/Jaffedrone-Manual.pdf "build/mux-$*/$(MUX_APP)/"
	echo $* > "build/mux-$*/$(MUX_APP)/DEVICE" # which build: in sysinfo.txt
	chmod +x "build/mux-$*/$(MUX_APP)/launch.sh" "build/mux-$*/$(MUX_APP)/sysinfo.sh" \
		"build/mux-$*/$(MUX_APP)/jaffedrone" "build/mux-$*/$(MUX_APP)/mux_launch.sh" "build/mux-$*/$(MUX_APP)/wake_screen.sh"
	cd build/mux-$* && zip -qr ../Jaffedrone-mustardos-$*.muxapp "$(MUX_APP)"

package: build/Jaffedrone-mustardos-brick.muxapp build/Jaffedrone-mustardos-brickpro.muxapp

DEVICE ?= brick
INSTALL_DIR = $(SD)/MUOS/application/$(MUX_APP)
INSTALL_ROOT = $(SD)/MUOS
install: build/$(DEVICE)/jaffedrone $(APP)/splash.jdi $(APP)/LICENSE-mbedtls.txt
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/mounted/sdcard [DEVICE=brickpro]" && exit 1)
	@test -d "$(INSTALL_ROOT)" || (echo "$(INSTALL_ROOT) not found — is this the MustardOS card?" && exit 1)
	mkdir -p "$(INSTALL_DIR)"
	cp -r $(APP)/. "$(INSTALL_DIR)/"
	cp -r $(MUX)/. "$(INSTALL_DIR)/"
	cp build/$(DEVICE)/jaffedrone "$(INSTALL_DIR)/"
	echo $(DEVICE) > "$(INSTALL_DIR)/DEVICE"
	sync

clean:
	rm -rf build $(APP)/LICENSE-mbedtls.txt

build/version: FORCE
	@mkdir -p build
	@echo '$(JD_VERSION)' | cmp -s - $@ || echo '$(JD_VERSION)' > $@
FORCE:

# the app's version: JD_VERSION=... if given, else the one in src/engine.h
version:
ifneq ($(JD_VERSION),)
	@echo '$(JD_VERSION)'
else
	@sed -n 's/^#define JD_VERSION "\([^"]*\)".*/\1/p' src/engine.h
endif

.PHONY: version all test bench package install clean manual update-test FORCE

# host-side wifi sync interop check (run alongside HI5's)
build/jd_synctest: $(SRC) src/main_synctest.c $(HDR)
	@mkdir -p build
	cc $(CFLAGS) -o $@ $(SRC) src/main_synctest.c -lm

# the PDF manual, with the app's own screens on a drawn Brick
manual: build/jd_test tools/mkmanual.py
	mkdir -p build/manual/shots docs
	build/jd_test build/manual/shots > /dev/null
	JD_VERSION='$(JD_VERSION)' python3 tools/mkmanual.py build/manual/shots docs/Jaffedrone-Manual.pdf
