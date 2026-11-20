/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB audio for the TrimUI Brick and Brick Pro on MustardOS (SDL2, ALSA), made to be shared by the
 * sydilaxe TrimUI apps (TrimFX first; Conduit and the others to follow). Self-contained: SDL2 and
 * aring.c, no other part of the app.
 *
 * The app gives one function that turns stereo input into stereo output, 48 kHz float; this does
 * the rest:
 *   - picks the USB interface first, for input and output (by name, or by the kernel's list of
 *     USB-Audio cards, as many show only a product name), follows hot-plugging, keeps a choice made
 *     in SETUP across hot-plugs and restarts (audio.txt)
 *   - DIRECT: a USB interface is opened straight through ALSA's plughw (format conversion only),
 *     not the shared dmix / dsnoop path whose fixed 1024-frame periods set a latency floor of
 *     about 90 ms; the period then follows the buffer. Checked against the kernel after opening,
 *     with the shared path as the fallback. <app>_DIRECT=0 turns it off
 *   - opens it in 32-bit float with its native channels (from the kernel's stream0), so a 24- or
 *     32-bit interface keeps its bits and a multichannel one (a MixPre) offers its channel pairs
 *   - the input through aring: a small cushion, clock drift followed by resampling (no skips), mono
 *     to both sides
 *   - the audio threads at real-time priority (SCHED_FIFO)
 *   - the buffer: AUTO starts small (lowest latency), matches the interface's own period, and grows
 *     a step if the output keeps running dry; or a fixed size, remembered for each interface
 *   - one interface both ways (DIRECT): one clock, so a smaller input cushion
 *   - a watchdog: a device that stops calling back is opened again
 *   - optionally keeps the sound out of the internal speaker unless it is chosen (an FX send)
 *   - logs what the hardware really runs (format, rate, channels, period) and every decision
 * Costs: the ring's cubic interpolation, a few dozen operations a frame; nothing on the main thread
 * but a status check each frame. */
#pragma once
#include <SDL2/SDL.h>

typedef struct {
    const char *app;   /* settings prefix: <app>_BUFFER, <app>_AUDIO, <app>_INPUT */
    int speaker_guard; /* 1: with no USB output, the internal speaker stays silent unless chosen */
    /* the audio thread: stereo in (silence while there is none), stereo out, at the rate below */
    void (*process)(const float *in, float *out, int frames, int have_input, void *ud);
    void *ud;
    void (*log)(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    const char *settings; /* where to keep the choices (audio.txt), or NULL */
    /* optional: the output was (re)opened, with this many frames a callback (for the app's log) */
    void (*opened)(int frames, const char *device);
    /* new in brickio 0.1 (0 keeps the old behaviour) */
    int rate;          /* frames a second: 44100, 48000 (0: 48000), 88200 or 96000. On the direct path the
                        * interface must run it (no resampling there): one that can't uses the SDL path */
    int no_input;      /* 1: playback only (a synth): no input is opened, and a USB output runs straight
                        * through ALSA from one real-time thread (duplex.c), float to the interface's own
                        * 24- or 32-bit format at the very end */
} UaConfig;

/* first thing, before SDL_Init: the audio threads get cores of their own, and every thread started
 * from here on (SDL's own: its device watch, its video) keeps to the others. app: the settings prefix */
void ua_pin(const char *app);
/* the cores ua_pin gave the output and input threads (-1: none); 1 if pinned. An app with threads of
 * its own to place (a render-ahead thread, say) keeps them off these. <app>_CORES=out,in chooses them */
int ua_cores(int *out, int *in);
/* after SDL_Init(SDL_INIT_AUDIO): opens the devices */
void ua_start(const UaConfig *c);
/* each SDL event (hot-plug) */
void ua_event(const SDL_Event *e);
/* main thread, each frame: the watchdog, the buffer, the logs. Returns UA_NOTE_* bits for the app to
 * show (ua_note_text gives a line for each) */
enum { UA_NOTE_GUARD = 1, UA_NOTE_UNGUARD = 2, UA_NOTE_BUFFER = 4, UA_NOTE_REOPEN = 8 };
int ua_tick(void);
const char *ua_note_text(int note, char *buf, int n);
void ua_stop(void);

/* SETUP */
void ua_cycle_device(int capture, int dir); /* AUTO (USB first), then each device */
void ua_cycle_pair(int capture, int dir);   /* channels 1-2, 3-4... of a multichannel interface */
void ua_cycle_buffer(int dir);              /* AUTO, 64, 128, 256, 512, 1024, 2048 */

/* the round trip, measured: needs a cable from the output to the input (the effects go quiet for
 * about 2 s). ua_latency_result: 1 running, 2 done (ms set), -1 nothing came back, 0 not run */
void ua_latency_test(void);
int ua_latency_result(float *ms);

typedef struct {
    char in_name[128], out_name[128]; /* what is open ("" = none) */
    int in_auto, out_auto;            /* chosen automatically */
    int in_ch, out_ch, in_pair, out_pair;
    int buffer, buffer_auto;          /* frames */
    int guard;                        /* the speaker is being kept silent */
    int direct_in, direct_out;        /* opened through plughw, not the shared path */
    int duplex;                       /* the direct thread (duplex.c): one interface both ways, no cushion;
                                       * with no_input, the output alone (have_input 0) */
    int rate;                         /* frames a second */
    int have_input;
    int rt;                           /* the audio threads: 1 real-time, -1 refused, 0 not yet */
    char hw_in[64], hw_out[64];       /* "S24_3LE 48000 Hz 2 ch, period 1024" */
    unsigned underruns, skips, dropouts, in_clips, out_clips, reopens;
    unsigned cushion, margin;         /* frames */
    int drift_ppm;
    int exact;                        /* the input read sample for sample (one interface, one clock) */
    unsigned slips;                   /* exact: whole-frame corrections, crossfaded */
    float latency_ms;                 /* input cushion + the output's buffering, roughly */
} UaStatus;
void ua_status(UaStatus *s);
