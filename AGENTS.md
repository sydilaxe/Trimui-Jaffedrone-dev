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
effects on two pinned threads) and `src/midi.c` (the keys, their expression, the CCs and the MIDI page's clock
settings). Audio, MIDI I/O, the clock, the Brick link (a follower's bottom port as a USB gadget, driven by the
leader through usbfs), the audio clock, Wi-Fi sync and the performance log are `lib/brickio`, a copy of
sydilaxe/Brickio-dev's `src/` at the commit in `lib/brickio/BRICKIO_VERSION`: update it with Brickio-dev's
`tools/brickio-sync.sh`, never by editing it here (a change belongs in brickio, where every app gets it, and
where the link's bytes are checked so the apps keep linking to each other). The sound goes through brickio's
`usbaudio` (`bio_process`, reading the render ahead, or rendering under `bio_mx`); `JD_AUDIO_PATH=sdl` keeps the
old SDL path (`audio_cb`): keep both working. MIDI is brickio's reader and its musical clock wired by `mclockio`
(the MIDI page's IN DIV / OUT DIV and START OUT are `mclock_set_ratio` and `mclock_set_start_out`). The engine is shared with
the Miyoo version; keep `engine_render` and the split `engine_voices` / `engine_effects` the same sound,
sample for sample (the harness checks "render split ... same sound 1"). Nothing in the effects stage may
feed back into the voices.

`GP_MIDI` and `GP_MIDISTART` (the MIDI clock's way and START / STOP in, the MIDI page's CLOCK IN, CLOCK OUT and START IN switches) are the TrimUI's: they made the
song JDR18; the triplet, dotted and quintuple ring rates and `SP_ACCENT` made it JDR19, which loads every
older song (with ACCENT 0, so it sounds as it did). The Miyoo version has neither yet, so its loader rejects a
song saved here. Like WIFI SYNC and the output, they belong to the Brick: loading a preset keeps them.

The manual (`docs/Jaffedrone-Manual.pdf`, made by `tools/mkmanual.py` from the test's screenshots, on a
drawn Brick) ships in both packages. When a function, control or screen is added or changed, add or update its
section in `tools/mkmanual.py`, run `make manual` and commit the PDF with the change. CI fails when the
manual is older than its sources (`mkmanual.py --check`).

The app updates itself from this repo's latest GitHub release (`src/update.c`, `src/net.c` with Mbed TLS,
`dist/Jaffedrone/cacert.pem`; the same updater as Columbia Street's): releases must be full releases marked
Latest with a `v<number>...` tag higher than the last, and the package names and the `Jaffedrone/` folder in
them must stay as they are, or installed copies stop finding their updates. An update never writes the
user's files (`*.sav*`, `*.jds`, `*.cfg`, the logs): keep `keep_users` in `update.c` in step with any new
one. `make update-test` (CI runs it) checks it end to end. `mux_launch.sh` holds MustardOS's idle off while
the app is open (its Idle Mute would stop the sound two minutes after the last press); the block is the
same in Columbia Street, HI5, RadioScan and Conduit: keep the copies the same.

The log (`src/log.c`, `jaffedrone_log.txt`) and `dist/Jaffedrone/sysinfo.sh` (`sysinfo.txt`) are for
troubleshooting and tuning on the Brick and Brick Pro. `sysinfo.sh` is shared with HI5 and RadioScan; keep the
copies the same. The audio thread only fills counters; all writing happens in the main thread.

Only MustardOS is built for, on the Brick (and Brick Hammer) and the Brick Pro: no TrimUI OS or Knulli packages.
