/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The musical clock, wired: mclock (mclock.h) connected to brickio's MIDI ports (midi.h) and to the Bricks
 * on the USB clock link (link.h), so an app gets clock in and out, both ways at once, from one call.
 *
 *   in:   the clock bytes (F8 FA FB FC), song position (F2) and the link's position message (linkpos.h)
 *         go to mclock the moment they are read, on the reader's thread (midi_set_clock, midi_set_sysex):
 *         never through a queue the screen empties. A leading Brick's bytes (the follower's link_follow_start)
 *         come in through mcio_link_bytes the same way.
 *   out:  a real-time thread of its own steps mclock every millisecond: the clock, START and STOP to every
 *         MIDI port (leading; or following, with mclock_set_relay: then never to the port the clock comes
 *         in on), and to the Bricks following on the link (found and dropped as they come and go), with the
 *         link position every 20 ms.
 *   the app: every call that changes it (the ClockHost's set_*) is made holding the app's lock, the one its
 *         audio callback takes, so the host's functions are plain reads and writes. Reads (bpm, position,
 *         playing) are made without it, as mclock expects.
 *
 * Which way the clock goes is mclock's: mclock_set_mode / _transport / _relay / _latency, any time. Keep the
 * latency right (the output's real queue, ua_status), or run aclock in the audio callback: the clock out and
 * the phase lock are only as good as what they are told the heard sound is. */
#pragma once
#include <stdint.h>

#include "mclock.h"

typedef struct {
    const ClockHost *host;          /* the app (mclock_init) */
    void (*lock)(void);             /* held around every change to the app; NULL: none */
    void (*unlock)(void);
    int midi;                       /* 1: brickio's MIDI ports (the app starts midi_start itself, before or after) */
    int links;                      /* 1: lead Bricks following on the USB clock link */
    const char *sysfs;              /* where to look for them (NULL: /sys/bus/usb/devices) */
    int prio;                       /* the clock thread's SCHED_FIFO priority (0: 45) */
    void (*notice)(const char *msg);/* "LINK: A BRICK FOLLOWS THIS ONE", any thread; NULL: none */
} McioConfig;

/* mclock_init with the host, and the hooks on the MIDI reader; no thread yet. Enough for tests: feed it with
 * mcio_feed and step it with mclock_step */
void mcio_init(const McioConfig *c);
/* the clock thread (after mcio_init); 0 on success */
int mcio_start(void);
void mcio_stop(void);                /* STOP sent if the clock was going out; the links closed */

/* MIDI bytes from anywhere else (a test, another transport), with their arrival time: the clock, song
 * position and the link position are taken, the rest passed by. The parser keeps its place between calls */
void mcio_feed(const uint8_t *b, int n, uint64_t t_us);
/* the follower's side of the link: give this to link_follow_start as on_bytes */
void mcio_link_bytes(const uint8_t *b, int n, uint64_t t_us, void *ud);

int mcio_links(void);                /* Bricks following this one on the link now */
