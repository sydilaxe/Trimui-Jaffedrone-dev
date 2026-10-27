# Jaffedrone — 5-string drone instrument for Miyoo Mini Plus / OnionOS
#
#   make          cross-compile dist/Jaffedrone/jaffedrone for the Miyoo (ARMv7 hard-float, glibc 2.28)
#   make test     build the host harness and render screenshots + audio into build/test
#   make zip      package dist/Jaffedrone as build/Jaffedrone.zip for copying to SD:/App/
#   make install SD=/run/media/$USER/<card>   copy the app onto a mounted SD card
#   make package CROSS=<prefix>   build with the OnionOS gcc toolchain (no zig) and zip it as
#                 build/Jaffedrone-miyoo.zip, laid out to unzip at the root of the SD card (CI does this)

ZIG    ?= $(HOME)/.local/opt/zig-aarch64-linux-0.14.1/zig
TARGET := arm-linux-gnueabihf.2.28
SDL_INC := third_party/SDL-1.2.15-headers
SDL_LIB := third_party/onion-lib/libSDL-1.2.so.0

SRC    := src/engine.c src/ui.c src/gfx.c src/sync.c src/log.c
HDR    := src/engine.h src/ui.h src/gfx.h src/sync.h src/log.h src/font_data.h src/logo_data.h
CFLAGS := -O2 -std=gnu11 -Wall -Wextra -fno-math-errno
APP    := dist/Jaffedrone

all: $(APP)/jaffedrone $(APP)/icon.png $(APP)/splash.jdi

src/logo_data.h: tools/mklogo.py
	python3 tools/mklogo.py $@

src/font_data.h: tools/mkfont.py
	python3 tools/mkfont.py /usr/share/fonts/google-noto/NotoSansMono-Bold.ttf $@

$(APP)/icon.png: tools/mkicon.py
	python3 tools/mkicon.py $@

$(APP)/splash.jdi: tools/mksplash.py art/roofline.jpg src/logo_data.h src/ui.c
	python3 tools/mksplash.py art/roofline.jpg $@

# Onion's MI_AO/MI_GFX-backed SDL 1.2 (LGPL), linked against but not committed
SDL_URL := https://raw.githubusercontent.com/OnionUI/Onion/main/static/build/.tmp_update/lib/parasyte/libSDL-1.2.so.0
$(SDL_LIB):
	mkdir -p $(dir $@)
	curl -sSL -o $@ $(SDL_URL) || wget -qO $@ $(SDL_URL) || (rm -f $@; false)

$(APP)/jaffedrone: $(SRC) src/ahead.c src/main_sdl.c $(HDR) $(SDL_LIB)
	$(ZIG) cc -target $(TARGET) -mcpu=cortex_a7 $(CFLAGS) -I$(SDL_INC) -o $@ \
		$(SRC) src/ahead.c src/main_sdl.c $(SDL_LIB) -lm -lpthread -Wl,--allow-shlib-undefined -s

build/jd_test: $(SRC) src/ahead.c src/main_test.c $(HDR)
	@mkdir -p build
	cc $(CFLAGS) -o $@ $(SRC) src/ahead.c src/main_test.c -lm -lpthread

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

zip: all
	@mkdir -p build
	rm -f build/Jaffedrone.zip
	cd dist && zip -r ../build/Jaffedrone.zip Jaffedrone -x "Jaffedrone/*.sav*" "Jaffedrone/log.txt"

install: all
	@test -n "$(SD)" || (echo "usage: make install SD=/path/to/mounted/sdcard" && exit 1)
	@test -d "$(SD)/App" || (echo "$(SD)/App not found — is this the OnionOS card?" && exit 1)
	mkdir -p "$(SD)/App/Jaffedrone"
	cp -r $(APP)/. "$(SD)/App/Jaffedrone/"
	sync

clean:
	rm -rf build $(APP)/jaffedrone

.PHONY: all test bench zip install clean

# host-side wifi sync interop check (run alongside HI5's)
build/jd_synctest: $(SRC) src/main_synctest.c $(HDR)
	@mkdir -p build
	cc $(CFLAGS) -o $@ $(SRC) src/main_synctest.c -lm

# the OnionOS toolchain's gcc instead of zig, e.g. in the aemiii91/miyoomini-toolchain container:
#   make package CROSS=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-
CROSS ?= arm-linux-gnueabihf-
miyoo: $(SRC) src/ahead.c src/main_sdl.c $(HDR) $(SDL_LIB)
	$(CROSS)gcc -marm -mcpu=cortex-a7 -mfpu=neon-vfpv4 -mfloat-abi=hard $(CFLAGS) -I$(SDL_INC) -o $(APP)/jaffedrone \
		$(SRC) src/ahead.c src/main_sdl.c $(SDL_LIB) -lm -lpthread -Wl,--allow-shlib-undefined -s

package: miyoo $(APP)/splash.jdi docs/Jaffedrone-Manual.pdf
	rm -rf build/sd build/Jaffedrone-miyoo.zip
	mkdir -p build/sd/App
	cp -r $(APP) build/sd/App/
	rm -f build/sd/App/Jaffedrone/*.sav* build/sd/App/Jaffedrone/*.jds build/sd/App/Jaffedrone/log.txt \
		build/sd/App/Jaffedrone/jaffedrone_log.txt* build/sd/App/Jaffedrone/jaffedrone.cfg build/sd/App/Jaffedrone/jaffedrone_ui.cfg
	cp docs/Jaffedrone-Manual.pdf build/sd/App/Jaffedrone/
	chmod +x build/sd/App/Jaffedrone/launch.sh build/sd/App/Jaffedrone/jaffedrone
	cd build/sd && zip -qr ../Jaffedrone-miyoo.zip App

.PHONY: miyoo package manual

# the PDF manual, with the app's own screens on a drawn Miyoo
manual: build/jd_test tools/mkmanual.py
	mkdir -p build/manual/shots docs
	build/jd_test build/manual/shots > /dev/null
	python3 tools/mkmanual.py build/manual/shots docs/Jaffedrone-Manual.pdf
