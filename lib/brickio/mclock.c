/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The musical clock: see mclock.h. The logic is the sydilaxe apps' midi.c's, line for line where it
 * could be; what was song.bpm, live.playing and engine_* is the ClockHost. */
#include "mclock.h"

#include <math.h>
#include <stdatomic.h>
#include <stddef.h>

#include "aclock.h"
#include "linkpos.h"

static const ClockHost *H;
static atomic_int mode = MCLOCK_AUTO, transport = 1, relay, start_out = 1;
static _Atomic double in_r = 1.0, out_r = 1.0; /* the clock's ratio to the app's tempo, in and out */
static _Atomic float latency_s = 0.02f;

void mclock_set_mode(int m) { atomic_store(&mode, m < MCLOCK_AUTO || m > MCLOCK_OFF ? MCLOCK_AUTO : m); }
void mclock_set_transport(int on) { atomic_store(&transport, on != 0); }
void mclock_set_relay(int on) { atomic_store(&relay, on != 0); }
void mclock_set_start_out(int on) { atomic_store(&start_out, on != 0); }
static double ratio_ok(double r) { return r >= 0.125 && r <= 8.0 ? r : 1.0; }
void mclock_set_ratio(double in, double out) { atomic_store(&in_r, ratio_ok(in)), atomic_store(&out_r, ratio_ok(out)); }
void mclock_set_latency(float s) { atomic_store(&latency_s, s < 0.f ? 0.f : s); }

static int can_follow(void) { int m = atomic_load(&mode); return m == MCLOCK_AUTO || m == MCLOCK_FOLLOW; }
static int can_send(void) { int m = atomic_load(&mode); return m == MCLOCK_AUTO || m == MCLOCK_SEND; }
/* START, STOP, CONTINUE and song position too, unless set to the clock only */
static int can_transport(void) { return can_follow() && atomic_load(&transport); }

/* the app's position read while the audio may be moving it: the middle of three reads is whole */
static double position(void)
{
    double a = H->position(), b = H->position(), c = H->position();
    if (a > b) {
        double t = a;
        a = b, b = t;
    }
    return c < a ? a : c > b ? b : c;
}

/* where the sound is at the speaker: the audio clock, else the position less the output's latency */
static double heard_now(uint64_t t)
{
    double h;
    return aclock_position(t, &h) ? h : position() - atomic_load(&latency_s) * H->bpm() / 15.0;
}

/* ------------------------------------------------------------ clock in */

static uint64_t tick_us, ext_seen_us;
static double tick_avg_us;
static int64_t ticks;           /* the 16th / 6 the last tick marked: the first tick after START is 0 */
static double in_pos;           /* where the last tick has the app, in its own 16ths (each tick in_r / 6 on) */
static atomic_int following;    /* an outside clock leads */
static int ext_running;
static int settle;              /* clocks after START before the phase is held: until the first step has played */
static int snap;                /* CONTINUE: the next tick places the app exactly (wherever between ticks it came) */
static uint64_t precise_us;     /* the last position from a leading Brick: its tempo and phase win over the ticks' */
static double pll_i;

int mclock_following(void) { return atomic_load(&following); }

/* the last ticks' times, for the tempo: a straight line through four beats of them, which averages
 * out the jitter of USB and the thread waking */
#define TK 96
static uint64_t tk_t[TK];
static int tk_n, tk_w;

/* the microseconds a tick, from a straight line through the last n ticks' times */
static double fit(int cnt)
{
    uint64_t t0 = tk_t[(tk_w - cnt + TK) % TK];
    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, n = cnt;
    for (int i = 0; i < cnt; i++) {
        double y = (double)(tk_t[(tk_w - cnt + i + TK) % TK] - t0);
        sx += i, sy += y, sxx += (double)i * i, sxy += i * y;
    }
    double d = n * sxx - sx * sx;
    return d > 0.0 ? (n * sxy - sx * sy) / d : 0.0;
}

static void tk_push(uint64_t t)
{
    tk_t[tk_w] = t;
    tk_w = (tk_w + 1) % TK;
    if (tk_n < TK) tk_n++;
}

static void clock_tick(uint64_t t)
{
    if (tick_us && (t <= tick_us || t - tick_us >= 250000)) tk_n = 0; /* a gap: the fit starts afresh */
    /* ticks lost on the way (a busy USB hub, a full buffer): a gap of 2-4 ticks' length, within a third of
     * a tick, while the tempo is settled. Counted all the same, so the phase stays on the right tick, and
     * put where they were due, so the tempo isn't pulled down by them */
    int lost = 0;
    if (tick_us && tk_n >= 24 && tick_avg_us > 0.0) {
        double gap = (double)(t - tick_us), k = floor(gap / tick_avg_us + 0.5);
        if (k >= 2.0 && k <= 4.0 && fabs(gap - k * tick_avg_us) < tick_avg_us / 3.0) {
            lost = (int)k - 1;
            for (int m = 1; m <= lost; m++) tk_push(tick_us + (uint64_t)(gap * m / k));
        }
    }
    tk_push(t);
    /* a tempo change: the last beat's ticks (a short fit) disagree with the four beats' by more than
     * 1 %, far beyond what jitter does (a few hundredths of one): the four beats start again from that
     * beat, so the new tempo is taken in one beat rather than four (and the phase has less to catch up) */
    if (tk_n >= 48) {
        double s = fit(24);
        if (s > 0.0 && tick_avg_us > 0.0 && fabs(s - tick_avg_us) > 0.01 * tick_avg_us) tk_n = 24;
    }
    if (tk_n >= 2) {
        double slope = fit(tk_n);
        if (slope > 0.0) tick_avg_us = slope;
    }
    tick_us = t;
    ext_seen_us = t;
    if (!can_follow()) return;
    if (tick_avg_us <= 0.0) { /* the tempo isn't known yet (a clock that starts with START): count it all the same */
        if (ext_running && H->playing()) ticks++, in_pos += in_r / 6.0, settle -= settle > 0;
        return;
    }
    if (precise_us && t - precise_us < 1000000) { /* a leading Brick's positions hold the tempo and phase */
        atomic_store(&following, 1);
        ticks += 1 + lost;
        in_pos += (1 + lost) * in_r / 6.0;
        return;
    }
    atomic_store(&following, 1);
    /* the tempo: set when it has really changed (the phase lock below takes up what is left), more
     * finely while the fit is still filling (after START or a tempo change) */
    float bpm = (float)(60e6 / (tick_avg_us * 24.0) * in_r); /* the ratio: 0.5 half time */
    bpm = bpm < 40.f ? 40.f : bpm > 250.f ? 250.f : bpm;
    /* (in the clock's own terms, so a ratio follows as closely as 1:1). Once the fit is full (four beats),
     * the tempo is set from it once more, finely: an early, noisy fit's tempo isn't left for the phase lock
     * to carry; after that only a real change moves it, so jitter can't make it hunt */
    static int fit_full;
    if (tk_n < TK) fit_full = 0;
    float thr = (float)((tk_n < TK ? 0.05 : fit_full ? 0.15 : 0.005) * in_r);
    if (tk_n >= TK) fit_full = 1;
    if (fabsf(bpm - H->bpm()) > thr) {
        /* the phase lock's integral has been carrying the old tempo's error as a speed trim: hand it over,
         * so the speed heard doesn't change at the moment the tempo does (no wobble while it unwinds) */
        float was = H->bpm(), now = roundf(bpm * 1000.f) / 1000.f;
        if (was > 0.f) pll_i = fmax(-0.004, fmin(0.004, pll_i - (now / was - 1.0)));
        H->set_bpm(now);
    }
    if (!ext_running || !H->playing()) return;
    ticks += 1 + lost;
    in_pos += (1 + lost) * in_r / 6.0;
    if (snap) { /* the first tick after CONTINUE marks where the song goes on: the heard sound there now */
        snap = 0;
        H->set_position(position() + in_pos - heard_now(t));
        H->set_rate(1.0);
        pll_i = 0.0;
        return;
    }
    if (settle > 0) {
        /* the first step has played: the next tick places the app exactly, as after CONTINUE (its tempo
         * wasn't known for the first ticks, so it can be a good part of a tick out) */
        if (--settle == 0) snap = 1;
        return;
    }
    /* the phase: what is heard (the audio clock) should be where the clock is; trimmed gently,
     * jumped only when far off */
    double err = in_pos - heard_now(t), e = err / in_r; /* e: in the clock's 16ths, whatever the ratio */
    if (fabs(e) > 2.0) {
        H->set_position(position() + err);
        H->set_rate(1.0);
        pll_i = 0.0;
    } else {
        /* a little integral takes up the tempo's last difference, so the tempo needn't be set again for it.
         * Its gain is per second, not per tick (as at 120 BPM): ticks come twice as often at 240, and a
         * gain per tick twice as strong there wound the trim up after the start and rang for seconds */
        double k = 0.002 * fmin(2.0, tick_avg_us / 20833.3);
        pll_i = fmax(-0.001, fmin(0.001, pll_i + e * k));
        H->set_rate(1.0 + fmax(-0.02, fmin(0.02, e * 0.05 + pll_i)));
    }
}

void mclock_realtime(uint8_t b, uint64_t t)
{
    switch (b) {
    case 0xf8: clock_tick(t); break;
    case 0xfa: /* START: from the top. The first tick after it is the downbeat (MIDI 1.0), whether the
                * clock was running before (most drum machines) or starts with it (most sequencers) */
        if (!can_transport()) break;
        ticks = -1, in_pos = -in_r / 6.0;
        ext_running = 1;
        /* led from now: the clock out (AUTO) stays quiet even before the ticks have given a tempo, rather
         * than sending a START and a tick of its own back (mclock_watch lets go if no ticks follow) */
        atomic_store(&following, 1), ext_seen_us = t;
        settle = 6;
        H->set_rate(1.0);
        H->set_playing(0); /* the first step plays now */
        H->set_playing(1);
        break;
    case 0xfb: /* CONTINUE: from the song position (the last tick's, or a song position message's). The
                * app starts about there now, and the first tick after it places it exactly: CONTINUE can
                * come anywhere between two ticks of a clock that keeps running */
        if (!can_transport()) break;
        ext_running = 1;
        atomic_store(&following, 1), ext_seen_us = t;
        H->set_playing(1);
        if (ticks > 0) H->set_position(in_pos + in_r / 6.0 + atomic_load(&latency_s) * H->bpm() / 15.0), snap = 1;
        else settle = 6;
        break;
    case 0xfc: /* STOP */
        if (!can_transport()) break;
        ext_running = snap = 0;
        H->set_rate(1.0);
        H->set_playing(0);
        break;
    }
}

/* song position: the next tick is this 16th's first (ticks is the last tick's, as after START, where it
 * is -1); while playing, the position moves there now */
void mclock_song_position(int sixteenths)
{
    if (!can_transport()) return;
    ticks = (int64_t)sixteenths * 6 - 1;
    in_pos = (sixteenths - 1.0 / 6.0) * in_r; /* the clock's 16ths, in the app's */
    if (H->playing()) H->set_position(sixteenths * in_r + atomic_load(&latency_s) * H->bpm() / 15.0);
}

void mclock_link_position(uint64_t t, double lead, double bpm, int lead_playing)
{
    if (!can_follow()) return;
    ext_seen_us = precise_us = t;
    atomic_store(&following, 1);
    if (bpm >= 40.0 && bpm <= 250.0 && fabs(bpm - H->bpm()) > 1e-4) H->set_bpm((float)bpm);
    if (!H->playing()) {
        if (lead_playing && can_transport()) { /* joined mid-song: in at the leader's place */
            H->set_playing(1);
            H->set_position(lead + (LINK_TRANSIT_S + atomic_load(&latency_s)) * bpm / 15.0);
            ext_running = 1;
            pll_i = 0.0;
        }
        return;
    }
    if (!lead_playing) { /* the leader stopped (a leader passing on another's clock sends no STOP) */
        if (can_transport()) {
            ext_running = 0;
            H->set_rate(1.0);
            H->set_playing(0);
        }
        return;
    }
    double err = lead + LINK_TRANSIT_S * bpm / 15.0 - heard_now(t);
    if (!can_transport()) err -= 16.0 * floor((err + 8.0) / 16.0); /* the bar only: the nearest way round */
    if (fabs(err) > 0.5) {
        H->set_position(position() + err);
        H->set_rate(1.0);
        pll_i = 0.0;
        return;
    }
    pll_i = fmax(-0.003, fmin(0.003, pll_i + err * 0.02));
    H->set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.4 + pll_i)));
}

void mclock_watch(uint64_t t)
{
    if (atomic_load(&following) && (t - ext_seen_us > 1000000 || !can_follow())) { /* the outside clock stopped, or not to be followed */
        atomic_store(&following, 0);
        ext_running = 0;
        H->set_rate(1.0);
    }
}

/* ------------------------------------------------------------ clock out */

static atomic_int out_running;
static double out_est, out_r_was = 1.0; /* out_r_was: the ratio the ticks were counted at */
static int64_t out_sent;
static uint64_t out_t, link_sent_us;

int mclock_sending(void) { return atomic_load(&out_running); }

void mclock_reset(void)
{
    tick_us = ext_seen_us = 0;
    tick_avg_us = 0.0;
    ticks = 0, in_pos = 0.0;
    atomic_store(&following, 0);
    ext_running = settle = snap = 0;
    precise_us = 0, pll_i = 0.0;
    tk_n = tk_w = 0;
    atomic_store(&out_running, 0);
    link_sent_us = 0;
}

void mclock_init(const ClockHost *host)
{
    H = host;
    mclock_reset();
}

int mclock_step(uint64_t t, uint8_t *out, int max)
{
    int n = 0;
    /* leading: when allowed to send; following: when relaying (the clock heard here goes on) */
    int play = H->playing() && (atomic_load(&following) ? atomic_load(&relay) : can_send());
    if (!play) {
        if (atomic_load(&out_running) && atomic_load(&start_out) && n < max) out[n++] = 0xfc;
        atomic_store(&out_running, 0);
        return n;
    }
    double bpm = H->bpm(), r = atomic_load(&out_r); /* the ratio: 2 sends double time */
    /* where the sound is, through whichever output plays it (a USB audio interface's own buffering
     * included): the audio clock */
    double heard = heard_now(t);
    if (!atomic_load(&out_running)) { /* START now: the next tick is the first */
        atomic_store(&out_running, 1);
        out_est = heard;
        out_r_was = r;
        out_sent = (int64_t)floor(heard * 6.0 * r);
        out_t = t;
        if (atomic_load(&start_out) && n < max) out[n++] = 0xfa;
        return n;
    }
    /* the app's position moves in blocks: a smooth estimate between them, held to it */
    out_est += (double)(t - out_t) * 1e-6 * bpm / 15.0;
    out_t = t;
    double d = heard - out_est;
    out_est += fabs(d) > 1.0 ? d : d * 0.02;
    int64_t want = (int64_t)floor(out_est * 6.0 * r);
    if (r != out_r_was) out_r_was = r, out_sent = want; /* the ratio changed: count on from here at the new rate */
    if (want - out_sent > (r > 1.0 ? 4 * r : 4)) out_sent = want - 1; /* a jump (a new position): no burst of ticks */
    while (out_sent < want && n < max) {
        out[n++] = 0xf8;
        out_sent++;
    }
    if (want < out_sent) out_sent = want; /* moved back: wait for it */
    return n;
}

int mclock_link_step(uint64_t t, uint8_t *out, int max)
{
    if (!atomic_load(&out_running) && !atomic_load(&following)) {
        link_sent_us = 0;
        return 0;
    }
    if (link_sent_us && t - link_sent_us < 20000) return 0;
    link_sent_us = t;
    return linkpos_encode(out, max, heard_now(t), H->bpm(), H->playing());
}
