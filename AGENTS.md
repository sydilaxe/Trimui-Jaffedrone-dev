# AGENTS.md

Jaffedrone is a five-string drone instrument for the TrimUI Brick, Brick Hammer and Brick Pro running
MustardOS (Allwinner A133P: four Cortex-A53 cores, 1 GB, a 1024x768 screen), ported from the Miyoo Mini
Plus version (sydilaxe/Miyoo-Jaffedrone-dev). `make` cross-compiles two builds with zig (aarch64, glibc
2.28) against the device's SDL2: `build/brick/jaffedrone` and `build/brickpro/jaffedrone` (`-DJD_BRICKPRO`
adds the analog sticks). `make package` zips both as MustardOS application archives, the only packages built
(`Jaffedrone-mustardos-brick.muxapp` and `-brickpro.muxapp`: the app folder in `Jaffedrone/`, plus
`dist/mustardos/`: `mux_launch.sh`, `wake_screen.sh`, run through `JD_SCREEN_WAKE` once the window is up as
the Brick Pro's screen can stay dark on MustardOS, and the list glyph). `make test` renders screenshots and
audio on the host into `build/test`. Without zig on the path, `pip install ziglang==0.14.1`.

The platform layer is `src/main_trimui.c` (SDL2: video scaled 1.6x from the 640x480 pages, the buttons of
MustardOS's "muOS-Keys" controller (the Brick's or the Brick Pro's numbering, found at run time as in Columbia Street and HI5), USB
audio, the sticks), `src/ahead.c` (render ahead; on the TrimUI the voices and the
effects on two pinned threads), `src/midi.c` (USB MIDI through ALSA raw MIDI) and `src/link.c` (the clock link
between Bricks: a follower's bottom port as a USB gadget through configfs and FunctionFS, found and driven by
the leader through usbfs; keep it and the link's messages in `midi.c` the same as Columbia Street's, HI5's and
RadioScan's, so the apps link to each other). The engine is shared with
the Miyoo version; keep `engine_render` and the split `engine_voices` / `engine_effects` the same sound,
sample for sample (the harness checks "render split ... same sound 1"). Nothing in the effects stage may
feed back into the voices.

`GP_MIDI` and `GP_MIDISTART` (the MIDI clock's way and START / STOP, on MIX) are the TrimUI's: they made the
song JDR18, which loads every older song; the Miyoo version doesn't have them yet, so its loader rejects a
song saved here. Like WIFI SYNC and the output, they belong to the Brick: loading a preset keeps them.

The manual (`docs/Jaffedrone-Manual.pdf`, made by `tools/mkmanual.py` from the test's screenshots, on a
drawn Brick) ships in both packages. When a function, control or screen is added or changed, add or update its
section in `tools/mkmanual.py`, run `make manual` and commit the PDF with the change. CI fails when the
manual is older than its sources (`mkmanual.py --check`).

The log (`src/log.c`, `jaffedrone_log.txt`) and `dist/Jaffedrone/sysinfo.sh` (`sysinfo.txt`) are for
troubleshooting and tuning on the Brick and Brick Pro. `sysinfo.sh` is shared with HI5 and RadioScan; keep the
copies the same. The audio thread only fills counters; all writing happens in the main thread.

Only MustardOS is built for, on the Brick (and Brick Hammer) and the Brick Pro: no TrimUI OS or Knulli packages.
