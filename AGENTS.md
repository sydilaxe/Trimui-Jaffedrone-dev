# AGENTS.md

Jaffedrone is a five-string drone instrument for the TrimUI Brick, Brick Hammer and Brick Pro running
TrimUI OS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768 screen), ported from the Miyoo Mini
Plus version (sydilaxe/Miyoo-Jaffedrone-dev). `make` cross-compiles two builds with zig (aarch64, glibc
2.28) against the device's SDL2: `build/brick/jaffedrone` and `build/brickpro/jaffedrone` (`-DJD_BRICKPRO`
adds the analog sticks). `make package` zips both for `Apps/Jaffedrone`. `make test` renders screenshots and
audio on the host into `build/test`. Without zig on the path, `pip install ziglang==0.14.1`.

The platform layer is `src/main_trimui.c` (SDL2: video scaled 1.6x from the 640x480 pages, the TrimUI's
joystick buttons, USB audio, the sticks), `src/ahead.c` (render ahead; on the TrimUI the voices and the
effects on two pinned threads) and `src/midi.c` (USB MIDI through ALSA raw MIDI). The engine is shared with
the Miyoo version; keep `engine_render` and the split `engine_voices` / `engine_effects` the same sound,
sample for sample (the harness checks "render split ... same sound 1"). Nothing in the effects stage may
feed back into the voices.

The manual (`docs/Jaffedrone-Manual.pdf`, made by `tools/mkmanual.py` from the test's screenshots, on a
drawn Brick) ships in both zips. When a function, control or screen is added or changed, add or update its
section in `tools/mkmanual.py`, run `make manual` and commit the PDF with the change. CI fails when the
manual is older than its sources (`mkmanual.py --check`).
