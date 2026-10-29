# AGENTS.md

Jaffedrone is a five-string drone instrument for the TrimUI Brick, Brick Hammer and Brick Pro running
TrimUI OS or MustardOS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768 screen), ported from the Miyoo Mini
Plus version (sydilaxe/Miyoo-Jaffedrone-dev). `make` cross-compiles two builds with zig (aarch64, glibc
2.28) against the device's SDL2: `build/brick/jaffedrone` and `build/brickpro/jaffedrone` (`-DJD_BRICKPRO`
adds the analog sticks). `make package` zips both for TrimUI OS (`Apps/Jaffedrone`) and as MustardOS
application archives (`Jaffedrone-mustardos-*.muxapp`: the same app folder in `Jaffedrone/`, plus
`dist/mustardos/`: `mux_launch.sh`, `wake_screen.sh`, run through `JD_SCREEN_WAKE` once the window is up as
the Brick Pro's screen can stay dark on MustardOS, and the list glyph). `make test` renders screenshots and
audio on the host into `build/test`. Without zig on the path, `pip install ziglang==0.14.1`.

The platform layer is `src/main_trimui.c` (SDL2: video scaled 1.6x from the 640x480 pages, the buttons of
TrimUI OS's controller or MustardOS's "muOS-Keys" one, found at run time as in Columbia Street and HI5, USB
audio, the sticks), `src/ahead.c` (render ahead; on the TrimUI the voices and the
effects on two pinned threads) and `src/midi.c` (USB MIDI through ALSA raw MIDI). The engine is shared with
the Miyoo version; keep `engine_render` and the split `engine_voices` / `engine_effects` the same sound,
sample for sample (the harness checks "render split ... same sound 1"). Nothing in the effects stage may
feed back into the voices.

The manual (`docs/Jaffedrone-Manual.pdf`, made by `tools/mkmanual.py` from the test's screenshots, on a
drawn Brick) ships in both zips. When a function, control or screen is added or changed, add or update its
section in `tools/mkmanual.py`, run `make manual` and commit the PDF with the change. CI fails when the
manual is older than its sources (`mkmanual.py --check`).

The log (`src/log.c`, `jaffedrone_log.txt`) and `dist/Jaffedrone/sysinfo.sh` (`sysinfo.txt`) are for
troubleshooting and tuning on the Brick and Brick Pro. `sysinfo.sh` is shared with HI5 and RadioScan; keep the
copies the same. The audio thread only fills counters; all writing happens in the main thread.
