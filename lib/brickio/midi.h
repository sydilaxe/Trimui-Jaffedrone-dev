/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB MIDI for the TrimUI Brick / Brick Pro (part of brickio, made to be shared by the sydilaxe
 * TrimUI apps). Reads ALSA's raw MIDI devices (/dev/snd/midiC*D*), so no libasound to ship; works
 * alongside USB audio on the same port or hub.
 *
 *   - a reader thread: each message is parsed the moment it arrives (running status, SysEx skipped)
 *     and queued with its time; the app is woken (wake callback) so it can act at once rather than
 *     at its next screen frame
 *   - devices plugged in later are found (it looks every two seconds); ones pulled out are dropped
 *   - notes, CCs, program change, pitch bend, aftertouch, and the clock / start / stop /
 *     continue real-time messages (for tempo sync)
 *   - midi_send: bytes out to every device that takes them
 *   - midi_clock_out: the MIDI clock sent from a thread of its own, steady (absolute timetable)
 * Costs: a sleeping thread, woken only by MIDI and the two-second look for devices. */
#pragma once
#include <stdint.h>

enum { MIDI_CC, MIDI_NOTE, MIDI_PROGRAM, MIDI_BEND, MIDI_TOUCH, MIDI_CLOCK, MIDI_START, MIDI_CONTINUE, MIDI_STOP };

typedef struct {
    uint8_t type; /* MIDI_* */
    uint8_t ch;   /* 0..15 (channel messages) */
    uint8_t note; /* 1 for a note (num = note; val = velocity, 0 = off), else 0 (kept for older code) */
    uint8_t num;  /* CC number, note, program; pitch bend: the low 7 bits */
    uint8_t val;  /* CC value, velocity, pressure; pitch bend: the high 7 bits */
    uint64_t t_ns;/* when it was read (CLOCK_MONOTONIC) */
} MidiEv;

/* starts the reader thread. pattern: another device glob (tests: a FIFO), or NULL;
 * wake: called from the thread when messages are queued (e.g. push an SDL user event), or NULL */
int midi_start(const char *pattern, void (*wake)(void));
/* the messages queued since the last call, up to max (any thread, one reader) */
int midi_poll(MidiEv *out, int max);
/* bytes out to every open device that takes them; how many devices took them */
int midi_send(const uint8_t *bytes, int n);
/* the same to every device but one (dev, as the hooks number them: the port a clock came in on, so a
 * relayed clock never goes back to its source); -1 sends to all */
int midi_send_except(int dev, const uint8_t *bytes, int n);
/* clock out to every device: 24 a beat at bpm, from a thread of its own on an absolute timetable;
 * start (FA) as it begins, stop (FC) at 0. A new bpm takes effect from the next tick */
void midi_clock_out(float bpm);
/* how many devices are open, and the first one's name ("" if none) */
int midi_count(void);
const char *midi_name(void);
void midi_stop(void);
#define midi_close midi_stop /* the older name (TrimFX) */

/* ---------------------------------------------------------------- for an app with its own MIDI code
 * A raw MIDI port can be read by one opener only, so brickio opens the devices and an app that keeps
 * its own parser takes the bytes from here: every byte read, as read, on the reader thread (copy them
 * out; don't block). dev: which device (0..midi_count()-1). NULL stops it. midi_poll still works. */
void midi_set_raw(void (*fn)(int dev, const uint8_t *bytes, int n, uint64_t t_ns, void *ud), void *ud);

/* the reader thread at real-time priority (SCHED_FIFO prio, 1-99; 0: as other threads, the default), so
 * each message's time is taken the moment it arrives, not when the scheduler gets round to the thread
 * (the screen's work can delay a normal thread by milliseconds: a clock follower's tempo and phase come
 * from those times). Before midi_start; refused (no permission), the thread runs as before */
void midi_set_priority(int prio);

/* the clock as it arrives, on the reader thread, before anything is queued: each real-time byte (F8 clock,
 * FA start, FB continue, FC stop; songpos -1), and song position (b F2, songpos in 16ths). For a clock
 * follower (mclockio.h wires it to mclock): t_ns is when the read woke, the closest to the byte's arrival.
 * The real-time bytes still queue as MIDI_CLOCK ... events for midi_poll. NULL stops it */
void midi_set_clock(void (*fn)(int dev, uint8_t b, int songpos, uint64_t t_ns, void *ud), void *ud);

/* each complete SysEx (F0 ... F7, up to 64 bytes; longer ones are passed by), on the reader thread:
 * e.g. the clock link's position message (linkpos.h) from a MIDI port. NULL stops it. */
void midi_set_sysex(void (*fn)(int dev, const uint8_t *msg, int n, uint64_t t_ns, void *ud), void *ud);
