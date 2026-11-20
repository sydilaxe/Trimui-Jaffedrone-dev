/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The input's ring, between a USB capture callback and a playback callback that don't run in step
 * (part of usbaudio, made to be shared by the TrimUI apps). One writer (the capture thread), one
 * reader (the playback thread), lock-free.
 *
 *   - a cushion: the reader takes nothing until a capture buffer, its own buffer and a margin have
 *     built up, then exactly its frames each call
 *   - drift: the two clocks never quite agree (and differ a lot when input and output are two
 *     devices). The reader follows the cushion's average level with a slow control loop and reads
 *     a hair faster or slower (to +-0.4 %, with cubic interpolation), so the cushion stays put
 *     with no skips and no clicks
 *   - running dry: what is left fades out, the cushion builds up again with a bigger margin (a step
 *     the size of a capture buffer, 64-256 frames, to 4096); after every 30 s with no trouble the
 *     margin comes back down an eighth (to 64; one interface both ways: every 15 s, to 32), so the
 *     latency settles as low as the interface allows. A new device or buffer starts the margin again from 128
 *   - one interface both ways (locked): one clock, so no resampling at all: the input is read
 *     sample for sample (bit-exact, no interpolation). Should the level still creep (an interface
 *     whose two directions run on different clocks after all), it slips by whole frames (4 at most
 *     a buffer) under a crossfade, and if those slips add up to over 100 ppm it follows the drift as above instead
 *   - far too full (after a stall): skips back to the cushion with a crossfade
 *   - channels: the writer takes one pair from a multichannel interface (or both sides from a mono
 *     one) */
#pragma once
#include <stdatomic.h>
#include <stdint.h>

#define AR_FRAMES 32768 /* a power of two: 0.68 s at 48 kHz */

typedef struct {
    float buf[AR_FRAMES * 2];
    atomic_uint w;            /* frames written (the writer) */
    atomic_uint r;            /* frames the reader is past (published for the writer) */
    atomic_int reset;         /* set by the owner while the writer is stopped: the reader starts over */
    atomic_ullong w_time;     /* when the writer last wrote (ns): so the reader can tell where it is
                               * between its bursts, and steer on the true level, not a saw-tooth */
    /* the reader's own */
    double pos;               /* where it reads, in frames (absolute, fractional) */
    int primed, fade_in;
    double avg, integ;        /* the cushion's average level, the control loop's integral */
    double ratio;             /* frames read per frame made */
    /* the counters (atomic: read by the main thread) */
    atomic_uint underruns, skips, overflows, clips, fill, margin;
    atomic_int ppm;           /* the drift being followed, parts per million */
    double calm;              /* seconds since the last underrun (the reader's) */
    atomic_uint slips;        /* exact: whole-frame corrections made */
    atomic_int exact;         /* 1 while the input is read sample for sample */
    int slipping;             /* exact: stepping the level back to the target */
    int resample;             /* locked, but the clocks proved to differ: resample after all */
    double ex_t, ex_net, ex_tgt0; /* exact: seconds measured, frames slipped net, the target then */
    int rate;                 /* frames a second (set by the owner before use; 0: 48000) */
    atomic_int locked;        /* set by the owner: input and output are one interface (one clock, one
                               * period): the cushion can be half an output buffer smaller */
} ARing;

void ar_init(ARing *r);
/* the writer: frames of src_ch interleaved channels; pair 0 takes channels 1-2, 1 takes 3-4...
 * (past the last pair: the first) */
void ar_push(ARing *r, const float *src, int frames, int src_ch, int pair, uint64_t now_ns);
/* the reader: frames stereo frames into dst. in_block: the capture side's buffer, frames.
 * Returns 1 while sound comes from the input, 0 while priming or dry (dst is then silence). */
int ar_pull(ARing *r, float *dst, int frames, int in_block, uint64_t now_ns);
/* now_ns: a monotonic clock, the same for both sides (CLOCK_MONOTONIC) */
/* the owner, with the writer stopped (a new input device): the reader primes again */
void ar_restart(ARing *r);
