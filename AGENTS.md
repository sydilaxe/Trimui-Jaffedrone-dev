# AGENTS.md

Jaffedrone is a five-string drone instrument for the Miyoo Mini Plus running OnionOS.
`make` cross-compiles `dist/Jaffedrone/jaffedrone` with zig (ARMv7 hard-float, glibc 2.28)
against Onion's own SDL 1.2; `make test` renders screenshots and audio on the host into
`build/test`. The platform layer (gfx, font, SDL front-end, build) is shared in spirit with
the sibling HI5 project; the engine is its own.

Without zig, the Miyoo binary also builds with the OnionOS toolchain image
(`aemiii91/miyoomini-toolchain`, or `mirror.gcr.io/aemiii91/miyoomini-toolchain`) by calling its
`arm-linux-gnueabihf-gcc` on the same sources with `-marm -mcpu=cortex-a7 -mfpu=neon-vfpv4
-mfloat-abi=hard` and linking `third_party/onion-lib/libSDL-1.2.so.0`.

The manual (`docs/Jaffedrone-Manual.pdf`, made by `tools/mkmanual.py` from the test's
screenshots) ships in the Miyoo zip. When a function, control or screen is added or changed,
add or update its section in `tools/mkmanual.py`, run `make manual` and commit the PDF with
the change. CI fails when the manual is older than its sources (`mkmanual.py --check`).
