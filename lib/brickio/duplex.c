/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Full duplex on one USB interface: see duplex.h. */
#define _GNU_SOURCE
#include "duplex.h"

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---------------------------------------------------------------- libasound, loaded at run time */
typedef struct snd_pcm snd_pcm_t;
typedef struct snd_hw snd_pcm_hw_params_t;
typedef struct snd_sw snd_pcm_sw_params_t;
typedef unsigned long uframes;
typedef long sframes;
enum { ST_PLAYBACK = 0, ST_CAPTURE = 1, ACCESS_RW_INTERLEAVED = 3, FORMAT_FLOAT_LE = 14 };

static struct {
    void *lib;
    int (*open)(snd_pcm_t **, const char *, int, int);
    int (*close)(snd_pcm_t *);
    int (*hw_malloc)(snd_pcm_hw_params_t **);
    void (*hw_free)(snd_pcm_hw_params_t *);
    int (*hw_any)(snd_pcm_t *, snd_pcm_hw_params_t *);
    int (*hw_access)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
    int (*hw_format)(snd_pcm_t *, snd_pcm_hw_params_t *, int);
    int (*hw_channels)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
    int (*hw_resample)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned);
    int (*hw_rate)(snd_pcm_t *, snd_pcm_hw_params_t *, unsigned *, int *);
    int (*hw_period)(snd_pcm_t *, snd_pcm_hw_params_t *, uframes *, int *);
    int (*hw_buffer)(snd_pcm_t *, snd_pcm_hw_params_t *, uframes *);
    int (*hw_apply)(snd_pcm_t *, snd_pcm_hw_params_t *);
    int (*hw_get_period)(const snd_pcm_hw_params_t *, uframes *, int *);
    int (*hw_get_buffer)(const snd_pcm_hw_params_t *, uframes *);
    int (*sw_malloc)(snd_pcm_sw_params_t **);
    void (*sw_free)(snd_pcm_sw_params_t *);
    int (*sw_current)(snd_pcm_t *, snd_pcm_sw_params_t *);
    int (*sw_start)(snd_pcm_t *, snd_pcm_sw_params_t *, uframes);
    int (*sw_avail)(snd_pcm_t *, snd_pcm_sw_params_t *, uframes);
    int (*sw_apply)(snd_pcm_t *, snd_pcm_sw_params_t *);
    int (*prepare)(snd_pcm_t *);
    int (*start)(snd_pcm_t *);
    int (*drop)(snd_pcm_t *);
    int (*link)(snd_pcm_t *, snd_pcm_t *);
    int (*unlink)(snd_pcm_t *);
    sframes (*readi)(snd_pcm_t *, void *, uframes);
    sframes (*writei)(snd_pcm_t *, const void *, uframes);
    sframes (*avail)(snd_pcm_t *);
    int (*delay)(snd_pcm_t *, sframes *);
    int (*wait)(snd_pcm_t *, int);
    const char *(*strerror)(int);
} A;

static int load(char *err, int n)
{
    if (A.lib) return 0;
    void *l = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!l) { snprintf(err, (size_t)n, "no libasound (%s)", dlerror()); return -1; }
#define SYM(f, name) if (!(*(void **)&A.f = dlsym(l, name))) { snprintf(err, (size_t)n, "libasound lacks %s", name); dlclose(l); return -1; }
    SYM(open, "snd_pcm_open") SYM(close, "snd_pcm_close")
    SYM(hw_malloc, "snd_pcm_hw_params_malloc") SYM(hw_free, "snd_pcm_hw_params_free") SYM(hw_any, "snd_pcm_hw_params_any")
    SYM(hw_access, "snd_pcm_hw_params_set_access") SYM(hw_format, "snd_pcm_hw_params_set_format")
    SYM(hw_channels, "snd_pcm_hw_params_set_channels") SYM(hw_resample, "snd_pcm_hw_params_set_rate_resample")
    SYM(hw_rate, "snd_pcm_hw_params_set_rate_near") SYM(hw_period, "snd_pcm_hw_params_set_period_size_near")
    SYM(hw_buffer, "snd_pcm_hw_params_set_buffer_size_near") SYM(hw_apply, "snd_pcm_hw_params")
    SYM(hw_get_period, "snd_pcm_hw_params_get_period_size") SYM(hw_get_buffer, "snd_pcm_hw_params_get_buffer_size")
    SYM(sw_malloc, "snd_pcm_sw_params_malloc") SYM(sw_free, "snd_pcm_sw_params_free") SYM(sw_current, "snd_pcm_sw_params_current")
    SYM(sw_start, "snd_pcm_sw_params_set_start_threshold") SYM(sw_avail, "snd_pcm_sw_params_set_avail_min")
    SYM(sw_apply, "snd_pcm_sw_params")
    SYM(prepare, "snd_pcm_prepare") SYM(start, "snd_pcm_start") SYM(drop, "snd_pcm_drop")
    SYM(link, "snd_pcm_link") SYM(unlink, "snd_pcm_unlink")
    SYM(readi, "snd_pcm_readi") SYM(writei, "snd_pcm_writei") SYM(strerror, "snd_strerror")
    SYM(avail, "snd_pcm_avail") SYM(delay, "snd_pcm_delay") SYM(wait, "snd_pcm_wait")
#undef SYM
    A.lib = l;
    return 0;
}

/* ---------------------------------------------------------------- the streams */
static DxConfig cfg;
static snd_pcm_t *cap, *play;
static int frames, periods, linked;
static float *cbuf, *pbuf, *silence;
static pthread_t thr;
static int thr_on;
static atomic_int run, alive;
static atomic_uint xruns, done;
static atomic_int out_held = -1, in_held = -1; /* frames waiting in each direction, the driver's included */
static uframes out_size;                     /* the output's buffer as the interface gave it */
static int prime_on;                         /* after a start: the output kept fed until the input comes */

/* one direction: float, the interface's channels, 48 kHz exactly (no resampling here: an
 * interface that can't do 48 kHz stays on the SDL path, whose conversion is better), period;
 * the buffer: the output's kept to what it queues, the input's roomier (it costs no latency:
 * each period is read the moment it is in) */
static int setup(snd_pcm_t *p, int ch, unsigned want_rate, uframes *period, uframes buffer, uframes *got_buffer, char *err, int n, const char *what)
{
    snd_pcm_hw_params_t *hw = NULL;
    snd_pcm_sw_params_t *sw = NULL;
    unsigned rate = want_rate;
    int dir = 0, e;
    uframes buf = buffer;
    if ((e = A.hw_malloc(&hw)) < 0) goto fail;
    if ((e = A.hw_any(p, hw)) < 0 || (e = A.hw_access(p, hw, ACCESS_RW_INTERLEAVED)) < 0 || (e = A.hw_format(p, hw, FORMAT_FLOAT_LE)) < 0 ||
        (e = A.hw_channels(p, hw, (unsigned)ch)) < 0 || (e = A.hw_resample(p, hw, 0)) < 0 || (e = A.hw_rate(p, hw, &rate, &dir)) < 0)
        goto fail;
    if (rate != want_rate) { snprintf(err, (size_t)n, "%s: the interface runs %u Hz, not %u", what, rate, want_rate); A.hw_free(hw); return -1; }
    if ((e = A.hw_period(p, hw, period, &dir)) < 0 || (e = A.hw_buffer(p, hw, &buf)) < 0 || (e = A.hw_apply(p, hw)) < 0) goto fail;
    A.hw_get_period(hw, period, &dir);
    if (got_buffer) A.hw_get_buffer(hw, got_buffer);
    A.hw_free(hw), hw = NULL;
    if ((e = A.sw_malloc(&sw)) < 0 || (e = A.sw_current(p, sw)) < 0) goto fail;
    A.sw_start(p, sw, (uframes)1 << 30); /* never by itself: dx starts both together */
    A.sw_avail(p, sw, *period);
    if ((e = A.sw_apply(p, sw)) < 0) goto fail;
    A.sw_free(sw);
    return 0;
fail:
    snprintf(err, (size_t)n, "%s: %s", what, A.strerror(e));
    if (hw) A.hw_free(hw);
    if (sw) A.sw_free(sw);
    return -1;
}

/* both from the top: the output's buffer filled with silence, then both started together.
 *
 * With an input, prime() then keeps the output full until the input's first period comes in: a USB
 * interface's input can take longer than the output's buffer to start (the 1010music bluebox: tens of
 * ms), and before, the output ran dry waiting for it, which restarted both, which ran dry again: an
 * endless string of restarts. The output starts full, as it always did (starting it lower, to save a
 * period, left a full-speed interface's driver short at 128 frames and made it worse). */
static int restart(void)
{
    if (cap && linked) A.drop(cap);
    else {
        if (cap) A.drop(cap);
        A.drop(play);
    }
    if (A.prepare(play) < 0 || (cap && A.prepare(cap) < 0)) return -1;
    for (int k = 0; k < periods; k++)
        if (A.writei(play, silence, (uframes)frames) < 0) return -1;
    prime_on = cap != NULL;
    if (cap && linked) return A.start(cap);
    int e = A.start(play);
    return cap ? A.start(cap) : e;
}

static uint64_t mono_ms(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
}

/* until the input's first period is in: the output kept from running dry, a quarter period of
 * silence each time it falls below half a period. Only then: a start whose input comes in time runs
 * exactly as before (each quarter added is a quarter period more latency, for good: both directions
 * run on one clock). 0 once the input is in, <0 an error (an xrun: restart, as is a second with no
 * input at all) */
static int prime(void)
{
    const sframes want = frames / 2, step = frames >= 64 ? frames / 4 : frames;
    const uint64_t t0 = mono_ms();
    for (;;) {
        sframes ca = A.avail(cap);
        if (ca < 0) return (int)ca;
        if (ca >= frames) return 0;
        sframes pa = A.avail(play);
        if (pa < 0) return (int)pa;
        if (pa >= step && (sframes)out_size - pa < want) {
            sframes w = A.writei(play, silence, (uframes)step);
            if (w < 0) return (int)w;
            continue;
        }
        if (!atomic_load_explicit(&run, memory_order_relaxed)) return -EINTR; /* dx_stop */
        if (mono_ms() - t0 > 1000) return -EPIPE;
        A.wait(cap, 1);
    }
}

static int gone(int e) { return e == -ENODEV || e == -EBADFD || e == -EIO || e == -ENOENT; }

static void *loop(void *arg)
{
    (void)arg;
    if (cfg.on_thread) cfg.on_thread();
    unsigned n = 0;
    while (atomic_load_explicit(&run, memory_order_relaxed)) {
        int got = 0, e = 0;
        if (prime_on) {
            prime_on = 0;
            e = prime();
            if (e == -EINTR || e == -EAGAIN) prime_on = 1; /* again, unless stopping */
        }
        /* a whole period in (playback only: none; the output's own pace times the loop) */
        while (!e && cap && got < frames) {
            sframes r = A.readi(cap, cbuf + (size_t)got * (size_t)cfg.ch_in, (uframes)(frames - got));
            if (r < 0) { e = (int)r; break; }
            got += (int)r;
        }
        if (!e) {
            cfg.period(cbuf, pbuf, frames, cfg.ud);
            int put = 0;
            while (put < frames) {
                sframes w = A.writei(play, pbuf + (size_t)put * (size_t)cfg.ch_out, (uframes)(frames - put));
                if (w < 0) { e = (int)w; break; }
                put += (int)w;
            }
        }
        if (e == -EINTR || e == -EAGAIN) continue;
        if (e) {
            if (gone(e) || !atomic_load(&run)) break;
            atomic_fetch_add(&xruns, 1); /* -EPIPE / -ESTRPIPE: a period missed */
            if (restart() < 0) break;
            continue;
        }
        atomic_fetch_add_explicit(&done, 1, memory_order_relaxed);
        if ((n++ & 63) == 0) { /* now and then: what each direction holds, for the log */
            sframes d;
            if (A.delay(play, &d) == 0) atomic_store_explicit(&out_held, (int)d, memory_order_relaxed);
            if (cap && A.delay(cap, &d) == 0) atomic_store_explicit(&in_held, (int)d, memory_order_relaxed);
        }
    }
    atomic_store(&alive, 0);
    return NULL;
}

static void close_all(void)
{
    if (cap && linked) A.unlink(cap);
    if (cap) A.close(cap), cap = NULL;
    if (play) A.close(play), play = NULL;
    free(cbuf), free(pbuf), free(silence);
    cbuf = pbuf = silence = NULL;
    linked = 0;
}

int dx_start(const DxConfig *c, char *err, int n)
{
    dx_stop();
    if (load(err, n) < 0) return -1;
    cfg = *c;
    periods = c->periods >= 3 ? 3 : 2;
    char dev[96];
    if (!strncmp(c->card, "pcm:", 4)) snprintf(dev, sizeof dev, "%s", c->card + 4); /* (tests: any PCM, e.g. "pcm:null") */
    else snprintf(dev, sizeof dev, "plughw:CARD=%s,DEV=0", c->card);
    int e;
    if ((e = A.open(&play, dev, ST_PLAYBACK, 0)) < 0) { snprintf(err, (size_t)n, "output %s: %s", dev, A.strerror(e)); play = NULL; close_all(); return -1; }
    if (c->ch_in > 0 && (e = A.open(&cap, dev, ST_CAPTURE, 0)) < 0) { snprintf(err, (size_t)n, "input %s: %s", dev, A.strerror(e)); cap = NULL; close_all(); return -1; }
    unsigned rate = c->rate > 0 ? (unsigned)c->rate : 48000u;
    uframes pp = (uframes)c->frames, cp = (uframes)c->frames;
    out_size = 0;
    if (setup(play, c->ch_out, rate, &pp, pp * (uframes)periods, &out_size, err, n, "output") < 0 ||
        (cap && setup(cap, c->ch_in, rate, &cp, cp * 8, NULL, err, n, "input") < 0)) {
        close_all();
        return -1;
    }
    if (!cap) cp = pp;
    if (pp != cp) { snprintf(err, (size_t)n, "the periods differ (output %lu, input %lu)", pp, cp); close_all(); return -1; }
    frames = (int)pp;
    if (!out_size) out_size = pp * (uframes)periods;
    cbuf = calloc((size_t)frames * (size_t)(c->ch_in > 0 ? c->ch_in : 1), sizeof(float));
    pbuf = calloc((size_t)frames * (size_t)c->ch_out, sizeof(float));
    silence = calloc((size_t)frames * (size_t)c->ch_out, sizeof(float));
    if (!cbuf || !pbuf || !silence) { snprintf(err, (size_t)n, "out of memory"); close_all(); return -1; }
    linked = cap && A.link(cap, play) == 0;
    atomic_store(&xruns, 0), atomic_store(&done, 0), atomic_store(&out_held, -1), atomic_store(&in_held, -1);
    if (restart() < 0) { snprintf(err, (size_t)n, "could not start the streams"); close_all(); return -1; }
    atomic_store(&run, 1), atomic_store(&alive, 1);
    if (pthread_create(&thr, NULL, loop, NULL) == 0) thr_on = 1;
    else {
        atomic_store(&run, 0), atomic_store(&alive, 0);
        snprintf(err, (size_t)n, "no thread");
        close_all();
        return -1;
    }
    if (cfg.log)
    {
        if (cap)
            cfg.log("AUDIO", "DUPLEX on %s: %u Hz, %d frames a period, %d queued out, %s; round trip ~%.1f ms + the interface", dev, rate, frames,
                    periods, linked ? "linked (one start)" : "not linked", (double)frames * (1 + periods) * 1000.0 / rate);
        else
            cfg.log("AUDIO", "PLAYBACK on %s: %u Hz, %d frames a period, %d queued; out in ~%.1f ms + the interface", dev, rate, frames, periods,
                    (double)frames * periods * 1000.0 / rate);
    }
    return 0;
}

void dx_stop(void)
{
    if (!cap && !play) return;
    /* the thread wakes within a period (the streams keep running) and sees run 0 */
    atomic_store(&run, 0);
    if (thr_on) pthread_join(thr, NULL), thr_on = 0;
    atomic_store(&alive, 0);
    close_all();
}

int dx_alive(void) { return atomic_load(&alive); }
int dx_frames(void) { return frames; }
unsigned dx_xruns(void) { return atomic_load(&xruns); }
unsigned dx_periods_done(void) { return atomic_load_explicit(&done, memory_order_relaxed); }
int dx_held(int *out, int *in)
{
    int o = atomic_load_explicit(&out_held, memory_order_relaxed), i = atomic_load_explicit(&in_held, memory_order_relaxed);
    if (out) *out = o;
    if (in) *in = i;
    return o >= 0;
}
