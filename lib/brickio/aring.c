/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "aring.h"

#include <math.h>
#include <string.h>

#define MASK (AR_FRAMES - 1)
#define MAX_DEV 0.004  /* the farthest the reader runs from the writer's rate: 4000 ppm */
#define XF 64          /* crossfades and fades, frames */

void ar_init(ARing *r)
{
    memset(r, 0, sizeof *r);
    r->ratio = 1.0;
    atomic_store(&r->margin, 128);
}

void ar_restart(ARing *r) { atomic_store_explicit(&r->reset, 1, memory_order_release); }

void ar_push(ARing *r, const float *src, int frames, int src_ch, int pair, uint64_t now_ns)
{
    unsigned w = atomic_load_explicit(&r->w, memory_order_relaxed), rd = atomic_load_explicit(&r->r, memory_order_acquire);
    if (src_ch < 1) return;
    int a = pair * 2, b = a + 1;
    if (b >= src_ch) a = 0, b = src_ch > 1 ? 1 : 0; /* a pair the interface hasn't: the first; mono: both */
    int clips = 0;
    for (int i = 0; i < frames; i++) {
        if (w - rd >= AR_FRAMES - 8) { atomic_fetch_add(&r->overflows, 1); break; } /* the reader stopped */
        float x = src[i * src_ch + a], y = src[i * src_ch + b];
        if (x != x) x = 0.f; /* never a NaN */
        if (y != y) y = 0.f;
        if (fabsf(x) >= 0.9999f || fabsf(y) >= 0.9999f) clips++;
        r->buf[2 * (w & MASK)] = x;
        r->buf[2 * (w & MASK) + 1] = y;
        w++;
    }
    atomic_store_explicit(&r->w_time, now_ns, memory_order_relaxed);
    atomic_store_explicit(&r->w, w, memory_order_release);
    if (clips) atomic_fetch_add(&r->clips, (unsigned)clips);
}

/* cubic (Catmull-Rom) between frames i and i+1, at t */
static inline float cubic(const float *b, unsigned i, int c, float t)
{
    float y0 = b[2 * ((i - 1) & MASK) + c], y1 = b[2 * (i & MASK) + c], y2 = b[2 * ((i + 1) & MASK) + c], y3 = b[2 * ((i + 2) & MASK) + c];
    return y1 + 0.5f * t * (y2 - y0 + t * (2.f * y0 - 5.f * y1 + 4.f * y2 - y3 + t * (3.f * (y1 - y2) + y3 - y0)));
}

int ar_pull(ARing *r, float *dst, int frames, int in_block, uint64_t now_ns)
{
    unsigned w = atomic_load_explicit(&r->w, memory_order_acquire);
    if (atomic_exchange_explicit(&r->reset, 0, memory_order_acq_rel)) {
        r->pos = (double)w, r->primed = 0, r->avg = 0.0, r->integ = 0.0, r->ratio = 1.0, r->calm = 0.0;
        r->resample = 0, r->ex_t = 0.0, r->slipping = 0;
        atomic_store(&r->r, w);
        atomic_store(&r->margin, 128); /* a new device or buffer: what was learnt about the old one doesn't apply */
    }
    unsigned margin = atomic_load_explicit(&r->margin, memory_order_relaxed);
    if (in_block < 64) in_block = 64;
    /* running dry adds a step the size of a capture buffer (64-256 frames): small buffers, small steps */
    unsigned step = in_block < 64 ? 64 : in_block > 256 ? 256 : (unsigned)in_block;
    /* the smallest cushion that never runs dry when the capture side arrives in in_block bursts and
     * the reader takes frames at a time: on average half a burst and one and a half reads, plus the
     * margin (jitter) and a reserve to fade out over */
    double reads = atomic_load_explicit(&r->locked, memory_order_relaxed) ? 1.0 : 1.5;
    double target = 0.5 * in_block + reads * frames + (double)margin + XF + 4.0;
    if (target > AR_FRAMES / 2) target = AR_FRAMES / 2;
    double fill = (double)w - r->pos;
    atomic_store_explicit(&r->fill, fill > 0 ? (unsigned)fill : 0u, memory_order_relaxed);

    if (!r->primed) {
        if (fill >= target) {
            r->primed = 1, r->fade_in = 1, r->avg = target, r->integ = 0.0, r->ratio = 1.0;
        } else {
            memset(dst, 0, sizeof(float) * 2 * (size_t)frames);
            if (fill < 0) r->pos = (double)w; /* (after a restart that raced the writer) */
            atomic_store(&r->r, (unsigned)r->pos);
            return 0;
        }
    }
    double need = frames * r->ratio + 4.0;
    if (fill < need + XF) {
        /* running dry: play out what there is (at least the fade's reserve, faded) and build a
         * bigger cushion */
        atomic_fetch_add(&r->underruns, 1);
        r->ex_t = 0.0; /* exact: a disturbance, not drift: measure again from here */
        if (margin < 4096) atomic_store(&r->margin, margin + step);
        r->calm = 0.0;
        int got = fill > 3.0 ? (int)(fill - 3.0) : 0;
        if (got > frames) got = frames;
        unsigned p = (unsigned)r->pos;
        for (int i = 0; i < got; i++) {
            float g = (float)(got - i) / (float)(got + 1);
            dst[2 * i] = r->buf[2 * ((p + (unsigned)i) & MASK)] * g;
            dst[2 * i + 1] = r->buf[2 * ((p + (unsigned)i) & MASK) + 1] * g;
        }
        for (int i = got; i < frames; i++) dst[2 * i] = dst[2 * i + 1] = 0.f;
        r->pos = (double)w; /* what is left is from before the gap: drop it, prime on fresh input */
        r->primed = 0;
        atomic_store(&r->r, (unsigned)r->pos);
        return 0;
    }
    /* far too full (the reader was held up, or the writer burst): skip back to the cushion */
    double skip = 0.0;
    if (fill > target + 3.0 * in_block + frames + 1024.0) {
        skip = floor(fill - target);
        atomic_fetch_add(&r->skips, 1);
        r->ex_t = 0.0; /* exact: a burst, not drift: measure again from here */
        r->avg = target, r->integ = 0.0;
    }
    /* the drift loop: the cushion's level, averaged over about a second (the capture side arrives
     * in bursts, so the level saw-tooths), steers the read rate. A critically damped PI loop: the
     * level moves 48000 frames a second per unit of rate, so Kp = 2 w / 48000, Ki = w^2 / 48000,
     * with w = 2 pi / 40 s: it settles in under a minute, too slowly to be heard as pitch */
    const double SR = r->rate > 0 ? (double)r->rate : 48000.0;
    double dt = (double)frames / SR;
    r->calm += dt;
    /* calm: a little less latency, gently (an eighth of the margin, at least 16). One interface both
     * ways (one clock, one period) goes lower and sooner: to 32 frames, every 15 s */
    int lk = atomic_load_explicit(&r->locked, memory_order_relaxed);
    unsigned floor_m = lk ? 32u : 64u;
    if (r->calm > (lk ? 15.0 : 30.0) && margin > floor_m) {
        unsigned d = margin / 8 > 16 ? margin / 8 : 16;
        atomic_store(&r->margin, margin > d && margin - d > floor_m ? margin - d : floor_m);
        r->calm = 0.0;
    }
    /* the level as it would be with the input arriving smoothly: where the writer is by now, from
     * when it last wrote (at most a burst and a half on) */
    double since = (double)(int64_t)(now_ns - atomic_load_explicit(&r->w_time, memory_order_relaxed)) * SR * 1e-9;
    if (since < 0) since = 0;
    if (since > 1.5 * in_block) since = 1.5 * in_block;
    double level = fill + since - 0.5 * in_block;
    r->avg += (level - r->avg) * (1.0 - exp(-dt / 1.0));
    double err = r->avg - target;
    const double wn = 2.0 * 3.14159265358979 / 40.0, KP = 2.0 * wn / SR, KI = wn * wn / SR;
    r->integ += err * dt;
    double lim = MAX_DEV / KI;
    if (r->integ > lim) r->integ = lim;
    if (r->integ < -lim) r->integ = -lim;
    double dev = KP * err + KI * r->integ;
    if (dev > MAX_DEV) dev = MAX_DEV;
    if (dev < -MAX_DEV) dev = -MAX_DEV;
    /* one interface both ways: one clock, nothing to follow. Read sample for sample (the cubic at a
     * whole frame is that frame exactly); a level that creeps past 8-24 frames slips back in one go */
    int exact = atomic_load_explicit(&r->locked, memory_order_relaxed) && !r->resample;
    atomic_store_explicit(&r->exact, exact, memory_order_relaxed);
    if (exact) {
        dev = 0.0;
        r->integ = 0.0;
        r->pos = floor(r->pos + 0.5);
        /* measured from 5 s in (the level has settled): the slips' net, less what the target moved
         * by itself (the margin), is the drift; past 100 ppm the clocks really differ */
        r->ex_t += dt;
        if (r->ex_t < 5.0) r->ex_net = 0.0, r->ex_tgt0 = target;
        /* past the level's own wobble, it slips back in steps of at most 4 frames a buffer, each
         * crossfaded (a 4-frame step can't be heard), until it is within 2 frames again */
        if (fabs(err) > 8.0 + in_block / 16.0) r->slipping = 1;
        if (fabs(err) < 2.0) r->slipping = 0;
        if (skip == 0.0 && r->slipping) {
            skip = (double)lrint(fmax(-4.0, fmin(4.0, err))); /* + ahead (too full), - back: crossfaded below */
            if (r->pos + skip > (double)w - frames - 4.0) skip = 0.0;
            if (skip != 0.0) {
                r->avg -= skip;
                r->ex_net += skip;
                atomic_fetch_add(&r->slips, 1);
            }
        }
        double drift = r->ex_net + (target - r->ex_tgt0);
        if (r->ex_t > 15.0 && fabs(drift) > 100e-6 * SR * (r->ex_t - 5.0) + 16.0) r->resample = 1;
    }
    r->ratio = 1.0 + dev;
    atomic_store_explicit(&r->ppm, (int)lrint(dev * 1e6), memory_order_relaxed);

    double from = r->pos, to = r->pos + skip;
    for (int i = 0; i < frames; i++) {
        unsigned bi = (unsigned)to;
        float bt = (float)(to - floor(to));
        float L = cubic(r->buf, bi, 0, bt), R = cubic(r->buf, bi, 1, bt);
        if (skip != 0.0 && i < XF) {
            unsigned ai = (unsigned)from;
            float at = (float)(from - floor(from)), x = (float)i / XF;
            L = cubic(r->buf, ai, 0, at) * (1.f - x) + L * x;
            R = cubic(r->buf, ai, 1, at) * (1.f - x) + R * x;
        }
        if (r->fade_in && i < XF) { float x = (float)i / XF; L *= x; R *= x; }
        dst[2 * i] = L;
        dst[2 * i + 1] = R;
        from += r->ratio;
        to += r->ratio;
    }
    r->fade_in = 0;
    r->pos = to;
    atomic_store_explicit(&r->r, (unsigned)r->pos - 2u, memory_order_release); /* the cubic looks one back */
    return 1;
}
