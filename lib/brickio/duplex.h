/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Full duplex on one USB interface (part of brickio): its capture and playback opened straight
 * through ALSA (libasound, loaded at run time as SDL does: nothing to ship), linked so they start
 * together on the interface's one clock, and run by one real-time thread:
 *
 *   read one period from the input -> the effects -> write it to the output
 *
 * No cushion between two callbacks: the round trip is the input's period plus what the output keeps
 * queued (DX periods, 2 by default), about 3 periods: 4 ms at 64 frames, plus the interface's own
 * converters. An xrun (a period missed) restarts both streams together, silence first, and is
 * counted. The device gone (unplugged) ends the thread; the owner sees dx_alive() go 0. */
#pragma once

typedef struct {
    const char *card;  /* the ALSA card id ("desktop"): plughw:CARD=<card>,DEV=0 */
    int ch_in, ch_out; /* the interface's channels each way; ch_in 0: playback only (a synth) */
    int frames;        /* the period asked for (the interface may give another) */
    int periods;       /* what the output keeps queued, in periods: 2 (the least) or 3 */
    /* the real-time thread, once, as it starts (priority, core) */
    void (*on_thread)(void);
    /* each period: cap is ch_in interleaved, play ch_out interleaved, frames each (float) */
    void (*period)(const float *cap, float *play, int frames, void *ud);
    void *ud;
    void (*log)(const char *tag, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
    /* new in brickio 0.1, last so that older initialisers still line up (0 keeps the old behaviour) */
    int rate;          /* frames a second (0: 48000): the interface must run it, no resampling */
} DxConfig;

/* opens, configures and starts; 0 on success (why not in err otherwise) */
int dx_start(const DxConfig *c, char *err, int n);
void dx_stop(void);
int dx_alive(void);           /* the thread is running (0 once the device has gone) */
int dx_frames(void);          /* the period the interface gave */
unsigned dx_xruns(void);      /* periods missed since dx_start */
unsigned dx_periods_done(void);
/* what each direction holds now, in frames, the driver's own queue included (snd_pcm_delay, sampled
 * every 64 periods): out, from a frame written to it being heard; in, frames come in but not yet
 * read. 0 until known. Their sum plus a period is the round trip, the interface's converters apart */
int dx_held(int *out, int *in);
