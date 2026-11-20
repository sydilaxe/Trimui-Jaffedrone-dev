/* SPDX-License-Identifier: GPL-3.0-or-later */
/* brickio: USB audio, MIDI and sync for the sydilaxe TrimUI apps. One include for all of it; or take the
 * modules one at a time (each .c needs only its own .h, but for usbaudio.c: aring, duplex, SDL2). */
#pragma once
#define BRICKIO_VERSION "0.2.0"
#define BRICKIO_VERSION_NUM 200 /* major * 10000 + minor * 100 + patch */

#include "aclock.h"   /* the audio clock: where the music is at the speaker, at any moment */
#include "aring.h"    /* the input's ring between two callbacks: cushion, drift, resampling */
#include "blink.h"    /* the Brick link's data channel: its frames */
#include "blinkio.h"  /* the Brick link's data channel: sessions, audio and MIDI between Bricks */
#include "blinkauto.h" /* the data channel managed: every Brick on the hub, or the leader, from the audio thread */
#include "duplex.h"   /* one interface straight through ALSA: full duplex, or playback only */
#include "link.h"     /* the clock link between Bricks over USB (the bottom port as a device) */
#include "linkpos.h"  /* the link's position message (F0 7D 'T' 'L') */
#include "mclock.h"   /* the musical clock: MIDI clock and the link, in and out, on one timeline */
#include "mclockio.h" /* the musical clock wired to the MIDI ports and the link: one call, both ways */
#include "midi.h"     /* USB MIDI: the devices, a reader thread, clock out, raw tap, SysEx hook */
#include "perflog.h"  /* the performance log: STAT, PROC, JUMP, HINT, END */
#include "sync.h"     /* HI5S, the Wi-Fi sync: tempo, bar phase, transport over UDP */
#include "usbaudio.h" /* the audio devices: USB first, direct, real-time, the buffer, the watchdog */
