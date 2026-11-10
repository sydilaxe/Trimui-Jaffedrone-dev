/* Headless host harness: renders the default drone to a WAV, screenshots
 * every page, checks tuning, gate and wifi sync, and times the DSP.
 *
 * usage: jd_test OUTDIR */
#include <math.h>
#include <stddef.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aclock.h"
#include "link.h"
#include "ahead.h"
#include "midi.h"
#include "sticks.h"
#include "engine.h"
#include "gfx.h"
#include "log.h"
#include "perflog.h"
#include "sync.h"
#include "ui.h"

#define RATE 48000

static const char *test_dir = "."; /* where the results go, for blocks that use out for bytes */
static const char *out_dir = "."; /* where the results go (out is shadowed in places) */

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void write_wav(const char *path, const int16_t *pcm, int frames)
{
    FILE *f = fopen(path, "wb");
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, RATE);
    put32(f, RATE * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * 4);
    fwrite(pcm, 4, frames, f);
    fclose(f);
}

static void write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t rgb[3] = {fb[i] >> 16, fb[i] >> 8, fb[i]};
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static void shot(const char *out, const char *name, uint32_t *fb)
{
    char path[512];
    ui_draw(fb);
    snprintf(path, sizeof path, "%s/%s.ppm", out, name);
    write_ppm(path, fb);
}

static void press(int b)
{
    ui_button(b, 1);
    ui_button(b, 0);
}

/* R1: the next page */
static void next_page(void) { press(BTN_R1); }


/* rendering ahead (ahead.c), as on the Miyoo: an engine slowed to a given
 * share of real time, and the driver's habit of asking for two buffers back
 * to back, then waiting two buffers' time */
static double slow_share;
static double mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}
static void slow_render(int16_t *out, int frames)
{
    double t0 = mono_s();
    engine_render(out, frames);
    while (mono_s() - t0 < slow_share * frames / RATE)
        ;
}
static void sleep_until(double t)
{
    double d = t - mono_s();
    if (d > 0) {
        struct timespec ts = {(time_t)d, (long)((d - (time_t)d) * 1e9)};
        nanosleep(&ts, NULL);
    }
}
/* seconds of paired requests; returns how many second buffers of a pair
 * came later than one buffer after the pair began (the device ran dry) */
static int paired_requests(int use_ahead, double secs, int16_t *dst)
{
    const int B = 1024;
    const double per = (double)B / RATE;
    static int16_t buf[1024 * 2];
    int late = 0, n = (int)(secs / (2 * per));
    double start = mono_s() + 0.05;
    for (int k = 0; k < n; k++) {
        double t = start + k * 2 * per;
        sleep_until(t);
        for (int j = 0; j < 2; j++) {
            if (use_ahead)
                ahead_read(buf, B);
            else
                slow_render(buf, B);
            if (dst)
                memcpy(dst + (size_t)(2 * k + j) * B * 2, buf, sizeof buf);
        }
        late += mono_s() > t + per;
    }
    return late;
}

static double rms_of(const int16_t *pcm, int from, int n)
{
    double r = 0;
    for (int i = from; i < from + n; i++)
        r += (double)pcm[2 * i] * pcm[2 * i];
    return sqrt(r / n);
}

/* pitch of the left channel by autocorrelation near an expected pitch,
 * with a parabolic peak for sub-sample accuracy */
static double hz_near(const int16_t *pcm, int from, int n, double expect)
{
    int lo = (int)(RATE / expect * 0.85), hi = (int)(RATE / expect * 1.18), best = lo;
    double *c = malloc(sizeof(double) * (hi - lo + 1)), bc = -1e30;
    for (int lag = lo; lag <= hi; lag++) {
        double acc = 0;
        for (int i = from; i < from + n; i++)
            acc += (double)pcm[2 * i] * pcm[2 * (i + lag)];
        c[lag - lo] = acc;
        if (acc > bc)
            bc = acc, best = lag;
    }
    double off = 0;
    if (best > lo && best < hi) {
        double y0 = c[best - lo - 1], y1 = c[best - lo], y2 = c[best - lo + 1];
        off = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
    }
    free(c);
    return RATE / (best + off);
}

/* YIN pitch over 40 .. 1500 Hz (no expected pitch needed) */
static double hz_yin(const int16_t *pcm, int from, int n)
{
    static double d[RATE / 40 + 2];
    int maxlag = RATE / 40, minlag = RATE / 1500, best = -1;
    double run = 0;
    for (int lag = 1; lag <= maxlag; lag++) {
        double acc = 0;
        for (int i = from; i < from + n; i++) {
            double x = pcm[2 * i] - pcm[2 * (i + lag)];
            acc += x * x;
        }
        run += acc;
        d[lag] = run > 0 ? acc * lag / run : 1;
    }
    for (int lag = minlag; lag < maxlag; lag++)
        if (d[lag] < 0.15) {
            while (lag + 1 < maxlag && d[lag + 1] < d[lag])
                lag++;
            best = lag;
            break;
        }
    if (best < 0) {
        double mn = 1e9;
        for (int lag = minlag; lag < maxlag; lag++)
            if (d[lag] < mn)
                mn = d[lag], best = lag;
    }
    double y0 = d[best - 1], y1 = d[best], y2 = d[best + 1], off = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2);
    if (!(off > -1 && off < 1))
        off = 0;
    return RATE / (best + off);
}

/* one wind (or string) voice alone: string 3 at a JUST MAJOR degree */
static void solo(int engine, int ex, int mat, int note, int breath)
{
    song_default();
    for (int s = 0; s < NS; s++)
        song.sp[s][SP_VOICE] = engine == ENG_WIND ? 1 + ex : 0;
    song.gp[GP_MATERIAL] = mat;
    engine_init(RATE);
    engine_set_playing(1);
    song.gp[GP_CHANGES] = 0;
    song.fx[FX_SCOUNT] = 0;
    song.fx[FX_DMIX] = song.fx[FX_VSEND] = song.fx[FX_BODY] = 0;
    song.gp[GP_DRIFT] = 0;
    song.lfo[0][LP_DEPTH] = 0;
    song.gp[GP_CUTOFF] = 100;
    song.gp[GP_GLIDE] = 0;
    for (int s = 0; s < NS; s++)
        song.on[s] = s == 2, song.sp[s][SP_PON] = song.sp[s][SP_BON] = song.sp[s][SP_FINE] = 0;
    song.sp[2][SP_BOW] = breath;
    song.sp[2][SP_NOTE] = note;
}

/* Wi-Fi sync over a simulated network: a leader 1.7 steps ahead whose
 * packets arrive 3 ms late at best and up to 40 ms later at random (one in
 * twenty held 100 ms, as for a power-saving unit), read only every 33 ms,
 * with the audio made in 1024-frame buffers. echo: the leader echoes our
 * packets (a current unit), or not (an older one). Returns how far the
 * follower is heard from the leader over the last 4 of 20 s, in ms. */
static unsigned sim_rand = 12345;
static double sim_jitter(void)
{
    sim_rand = sim_rand * 1103515245u + 12345u;
    double u = (sim_rand >> 8) / 16777216.0;
    return 3000.0 + (u < 0.05 ? 100000.0 : u * 40000.0);
}

typedef struct {
    uint8_t b[SYNC_PKT_LEN];
    uint64_t at;
    int live;
} SimPkt;

static void sim_sync(int echo, double *mean_ms, double *worst_ms, float *delay)
{
    const double bpm = 120.0, spu = bpm / 15e6; /* 16ths per us */
    const uint64_t off = 987654321ULL;           /* the leader's clock runs elsewhere */
    const int buf = 1024;
    song_default();
    engine_init(RATE);
    engine_set_bpm(97.f);
    song.gp[GP_SYNC] = SYNC_FOLLOW;
    sync_core_init(100);
    aclock_init(RATE, 0);
    static int16_t pcm[2 * 1024];
    SimPkt q[64];
    memset(q, 0, sizeof q);
    uint64_t heard_mine = 0, heard_mine_at = 0;
    double sum = 0, worst = 0;
    int n = 0;
    uint64_t next_cb = 0, next_poll = 0, next_lead = 7000, next_mine = 57000;
    for (uint64_t t = 0; t < 20000000ULL; t += 250) {
        double lead_pos = 1.7 + spu * (double)t, wire_pos = lead_pos + SYNC_WIRE_LEAD_US * spu;
        if (t >= next_cb) { /* the audio device asks for a buffer */
            aclock_request(t, buf);
            engine_render(pcm, buf);
            aclock_made(buf, engine_position(), engine_steps_per_frame());
            next_cb += (uint64_t)(buf * 1e6 / RATE);
        }
        if (t >= next_lead) { /* the leader sends */
            next_lead += 100000;
            for (int i = 0; i < 64; i++)
                if (!q[i].live) {
                    uint8_t *b = q[i].b;
                    uint64_t ts = t + off;
                    float fb = (float)bpm;
                    uint32_t id = 7;
                    memset(b, 0, sizeof q[i].b);
                    memcpy(b, "HI5S", 4);
                    b[4] = 1, b[5] = 1, b[6] = SYNC_LEAD;
                    memcpy(b + 8, &id, 4);
                    memcpy(b + 12, &fb, 4);
                    memcpy(b + 16, &wire_pos, 8);
                    memcpy(b + 24, &ts, 8);
                    if (echo && heard_mine_at && t - heard_mine_at < 1000000) {
                        uint32_t me = 100, hold = (uint32_t)(t - heard_mine_at);
                        b[7] = 1;
                        memcpy(b + 32, &me, 4);
                        memcpy(b + 36, &heard_mine, 8);
                        memcpy(b + 44, &hold, 4);
                    }
                    q[i].at = t + (uint64_t)sim_jitter();
                    q[i].live = 1;
                    break;
                }
        }
        if (t >= next_mine) { /* we send; the leader hears it a trip later */
            next_mine += 100000;
            uint8_t b[SYNC_PKT_LEN];
            sync_make_packet(b, t);
            memcpy(&heard_mine, b + 24, 8);
            heard_mine_at = t + (uint64_t)sim_jitter();
        }
        if (t >= next_poll) { /* the screen's frame reads what has come in */
            next_poll += 33000;
            for (int i = 0; i < 64; i++)
                if (q[i].live && q[i].at <= t) {
                    sync_handle_packet(q[i].b, SYNC_PKT_LEN, q[i].at);
                    q[i].live = 0;
                }
        }
        if (t >= 16000000ULL && t % 10000 == 0) {
            double mine;
            if (aclock_position(t, &mine)) {
                double e = fmod(lead_pos - mine, 16.0);
                if (e > 8)
                    e -= 16;
                if (e <= -8)
                    e += 16;
                e = fabs(e / spu / 1000.0);
                sum += e;
                worst = e > worst ? e : worst;
                n++;
            }
        }
    }
    *mean_ms = n ? sum / n : 1e9;
    *worst_ms = worst;
    *delay = sync_status()->delay_ms;
    engine_set_rate(1.0);
    sync_core_init(1);
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--midi-chart")) { /* the CC table of docs/MIDI.md */
        song_default();
        ui_midi_chart(stdout);
        return 0;
    }
    const char *out = argc > 1 ? argv[1] : ".";
    test_dir = out;
    out_dir = out;
    char path[512];
    static uint32_t fb[SCREEN_W * SCREEN_H];
    ui_set_dir(out);
    song_default();
    engine_init(RATE);
    engine_set_playing(1);
    ui_init();
    shot(out, "splash", fb);
    press(BTN_A); /* a press clears the start-up screen and does nothing else */

    const int secs = 12, chunk = RATE / 100;
    int16_t *pcm = malloc(sizeof(int16_t) * 2 * RATE * secs);
    static const char *const pages[PAGE_COUNT] = {"play", "sound", "compose", "mix", "song", "midi"};
    for (int t = 0, f = 0; t < secs * 100; t++, f += chunk) {
        engine_render(pcm + 2 * f, chunk);
        ui_tick(10);
        if (t == 300)
            press(BTN_DOWN), press(BTN_DOWN), press(BTN_X), press(BTN_X), press(BTN_X), press(BTN_SELECT), press(BTN_SELECT),
                press(BTN_SELECT), press(BTN_SELECT);
        if (t >= 320 && t < 320 + PAGE_COUNT * 10 && (t - 320) % 10 == 0) {
            shot(out, pages[(t - 320) / 10], fb);
            next_page();
        }
        if (t == 420) /* back on PLAY: the quiet view (START + R2), then hand-bow string 2 */
            ui_button(BTN_START, 1), press(BTN_R2), ui_button(BTN_START, 0);
        if (t == 470)
            shot(out, "quiet", fb);
        if (t == 480) {
            ui_button(BTN_START, 1), press(BTN_R2), ui_button(BTN_START, 0);
            press(BTN_RIGHT);
            ui_button(BTN_R2, 1); /* bow string 2 by hand */
        }
        if (t == 560) {
            shot(out, "play_bow", fb);
            ui_button(BTN_R2, 0);
        }
    }
    /* the Brick Pro's right stick on SOUND: R3 maps FOLD of string 2, and
     * the stick plays it and HARM (the marks light while it moves them) */
    {
        ui_set_sticks(1);
        next_page(); /* SOUND */
        press(BTN_RIGHT);
        for (int i = 0; i < 6; i++)
            press(BTN_DOWN); /* VOICE, NOTE, HARM, BOW, DECAY, BRIGHT, FOLD */
        int mapped = ui_stick_map() && ui_stick_param() == SP_FOLD;
        ui_lock();
        engine_express_param(1, SP_FOLD, 1, 80);
        engine_express_param(1, SP_HARM, 1, 2);
        ui_unlock();
        engine_render(pcm, RATE / 2);
        ui_tick(10);
        char v[24];
        sparam_format(1, SP_FOLD, v, sizeof v);
        shot(out, "sound_stick", fb);
        printf("R3 on SOUND maps FOLD %d; the row shows the stick's %s (want 80: %d)\n", mapped, v, !strcmp(v, "80"));
        engine_express_param(1, SP_FOLD, 0, 0);
        engine_express_param(1, SP_HARM, 0, 0);
        for (int i = 0; i < 6; i++)
            press(BTN_UP);
        press(BTN_LEFT);
        for (int i = 1; i < PAGE_COUNT; i++)
            next_page(); /* back to PLAY */
        ui_set_stick_param(SP_BRIGHT);
        ui_set_sticks(0);
    }
    /* controls on PLAY, as on HI5 and RadioScan: A on/off, B fewer pulses
     * (wrapping), X rotate, Y new pattern; R1 pages without stopping the
     * drone; a plain START tap stops it; L2 plucks */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        for (int i = 0; i < PAGE_COUNT; i++)
            next_page(); /* all the way round, back to PLAY */
        int playing_after_pages = live.playing;
        press(BTN_LEFT); /* the screenshots above left string 2 selected */
        int *P = song.sp[0];
        press(BTN_UP); /* the pluck ring of string 1: E(1,4) */
        int on0 = P[SP_PON];
        press(BTN_A);
        int on1 = P[SP_PON];
        press(BTN_A);
        press(BTN_B); /* 1 pulse -> wraps to 4 */
        int wrapped = P[SP_PPULSES];
        press(BTN_B);
        int fewer = P[SP_PPULSES];
        press(BTN_X);
        int rot = P[SP_PROT];
        press(BTN_L2);
        engine_render(pcm, RATE / 100);
        float m = live.meter[0];
        press(BTN_START);
        printf("controls: pages keep drone %d; A %d->%d; B wraps 1->%d then %d; X rotate ->%d; L2 pluck meter %.3f; START tap -> playing %d\n",
               playing_after_pages, on0, on1, wrapped, fewer, rot, m, live.playing);
        engine_set_playing(1);

        /* hand playing touches only the selected string; a held R2 bows on
         * through a driver's quick release-and-press; an L2 glitch does not
         * pluck twice */
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_SCOUNT] = 0;
        for (int s = 0; s < NS; s++)
            song.sp[s][SP_PON] = song.sp[s][SP_BON] = 0, song.sp[s][SP_BOW] = 0;
        engine_render(pcm, RATE);
        press(BTN_RIGHT), press(BTN_RIGHT); /* string 3 (from string 1) */
        ui_button(BTN_R2, 1);
        float mn = 1, others = 0;
        for (int i = 0; i < 300; i++) { /* 3 s held, with a 20 ms glitch at 1.5 s */
            if (i == 150)
                ui_button(BTN_R2, 0);
            if (i == 152)
                ui_button(BTN_R2, 1);
            engine_render(pcm, RATE / 100);
            ui_tick(10);
            if (i > 60)
                mn = fminf(mn, live.meter[2]);
            for (int s = 0; s < NS; s++)
                if (s != 2)
                    others = fmaxf(others, live.meter[s]);
        }
        ui_button(BTN_R2, 0);
        for (int i = 0; i < 100; i++)
            engine_render(pcm, RATE / 100), ui_tick(10);
        float after = live.meter[2];
        int plucks = 0;
        float last = 0;
        ui_button(BTN_L2, 1);
        ui_button(BTN_L2, 0);
        ui_tick(10);
        ui_button(BTN_L2, 1); /* 10 ms later: a glitch */
        for (int i = 0; i < 30; i++) {
            engine_render(pcm, RATE / 100);
            ui_tick(10);
            float m = live.meter[2];
            if (m > last * 1.5f && m > 0.3f)
                plucks++;
            last = m;
        }
        ui_button(BTN_L2, 0);
        printf("hand: string 3 bowed by R2 stays at least %.3f through a glitch (others at most %.3f), %.3f a second after release; L2 + glitch -> %d pluck\n",
               mn, others, after, plucks);
    }

    /* the log: a short DETAIL session with presses, edits, chords, hand
     * playing and a simulated slow audio callback, then its summary */
    {
        char lp[512], cp[512];
        snprintf(lp, sizeof lp, "%s/session_log.txt", out);
        snprintf(cp, sizeof cp, "%s/session.cfg", out);
        remove(lp);
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        uint64_t ms = 1000;
        jdlog_set_clock(ms);
        jdlog_open(lp, cp);
        jdlog_set_level(LOG_DETAIL);
        AudioTiming at = {0};
        at.buffer_us = 21333;
        for (int i = 0; i < 300; i++) { /* 3 s */
            engine_render(pcm, RATE / 100);
            ui_tick(10);
            ms += 10;
            at.calls++;
            at.us_sum += i == 150 ? 30000 : 300; /* one slow callback */
            at.us_max = at.us_max > (i == 150 ? 30000u : 300u) ? at.us_max : (i == 150 ? 30000u : 300u);
            at.late += i == 150;
            jdlog_tick(ms, &at);
            if (i == 20)
                press(BTN_RIGHT), press(BTN_X), press(BTN_B);
            if (i == 40)
                press(BTN_R1); /* to SOUND */
            if (i >= 60 && i < 70)
                press(BTN_X); /* one parameter, ten times: one merged line */
            if (i == 120)
                press(BTN_SELECT);
            if (i == 140)
                press(BTN_L1); /* back to PLAY */
            if (i == 160)
                ui_button(BTN_R2, 1);
            if (i == 220)
                ui_button(BTN_R2, 0);
        }
        jdlog_set_clock(ms);
        jdlog_close();
        FILE *lf = fopen(lp, "r");
        int lines = 0, stat = 0, ev = 0, hint = 0, sum = 0, voice = 0, param = 0;
        char ln[256];
        while (lf && fgets(ln, sizeof ln, lf)) {
            lines++;
            stat += strstr(ln, " STAT ") != NULL;
            ev += strstr(ln, " EVENT ") != NULL;
            hint += strstr(ln, " HINT ") != NULL;
            sum += strstr(ln, " SUM ") != NULL;
            voice += strstr(ln, " VOICE ") != NULL;
            param += strstr(ln, "param ") != NULL;
        }
        if (lf)
            fclose(lf);
        printf("log: %d lines (%d status, %d voice, %d events incl. %d merged param lines, %d hints, %d summary)\n", lines, stat,
               voice, ev, param, hint, sum);
    }

    snprintf(path, sizeof path, "%s/drone.wav", out);
    write_wav(path, pcm, RATE * secs);
    printf("rms per second:");
    for (int s = 0; s < secs; s++)
        printf(" %.0f", rms_of(pcm, s * RATE, RATE));
    printf("\n");

    /* tuning: string 1 at A2 with a pure pluck, no bow, no drift or fx */
    song_default();
    engine_init(RATE);
    engine_set_playing(1);
    for (int s = 1; s < NS; s++)
        song.on[s] = 0;
    song.sp[0][SP_BOW] = 0;
    song.sp[0][SP_PON] = 0;
    song.gp[GP_CHANGES] = 0;
    song.gp[GP_GLIDE] = 0;
    song.fx[FX_SYMP] = song.fx[FX_BODY] = 0;
    song.gp[GP_DRIFT] = 0;
    song.gp[GP_CUTOFF] = 100;
    song.fx[FX_DMIX] = 0;
    song.fx[FX_VSEND] = 0;
    song.lfo[0][LP_DEPTH] = 0;
    engine_render(pcm, RATE / 10);
    engine_pluck(0);
    engine_render(pcm, RATE);
    printf("tuning: string 1 = %.2f Hz (want 110)\n", hz_near(pcm, RATE / 4, RATE / 4, 110));
    /* just intonation: degree 4 of JUST MAJOR is a pure fifth, 165 Hz */
    song.sp[0][SP_NOTE] = 4;
    engine_pluck(0);
    engine_render(pcm, RATE);
    printf("tuning: JUST MAJOR degree 4 = %.2f Hz (want 165.00, a pure 3/2)\n", hz_near(pcm, RATE / 4, RATE / 4, 165));
    song.sp[0][SP_NOTE] = 0;
    engine_pluck(0);
    engine_render(pcm, RATE / 2);

    /* glide: an octave up with GLIDE = 1 BAR (3.3 s at 72) slides, no pluck */
    song.sp[0][SP_BOW] = 50; /* keep it ringing without a pluck */
    engine_render(pcm, RATE);
    song.gp[GP_GLIDE] = 6;
    song.sp[0][SP_NOTE] = 7; /* an octave: seven degrees of JUST MAJOR */
    engine_render(pcm, RATE / 2);
    double g1 = hz_near(pcm, RATE / 4, RATE / 8, 150);
    engine_render(pcm, RATE * 5);
    double g2 = hz_near(pcm, RATE * 4, RATE / 2, 220);
    printf("glide 1 BAR: 0.4 s after the change %.1f Hz (between 110 and 220), after 5.5 s %.1f Hz (220), rms %.0f\n", g1, g2,
           rms_of(pcm, RATE * 4, RATE));
    song.gp[GP_GLIDE] = 0;
    song.sp[0][SP_NOTE] = 0;
    engine_render(pcm, RATE / 4);
    printf("glide OFF: 0.1 s after the change %.1f Hz (110)\n", hz_near(pcm, RATE / 8, RATE / 8, 110));

    /* mono voicing: every string sits on string 1's note */
    song_default();
    song.gp[GP_MODE] = MODE_MONO;
    char v[20];
    sparam_format(NS - 1, SP_NOTE, v, sizeof v);
    printf("mono: string %d note shows '%s'\n", NS, v);

    /* save compatibility: a v0.0.1 four-string file loads, string 5 gets defaults */
    {
        enum { NGP_OLD = 7, NSPO = 10, LEVELO = 2 }; /* before GLIDE; v0.0.3 string layout */
        int32_t old[1 + 4 + 4 * NSPO + NGP_OLD + NLFO * NLP + 9];
        memset(old, 0, sizeof old);
        old[0] = 0x4a445203; /* JDR3 */
        for (int s = 0; s < 4; s++)
            old[1 + s] = s != 1;
        old[1 + 4 + 1 * NSPO + 0] = 5;
        old[1 + 4 + 3 * NSPO + LEVELO] = 77;
        old[1 + 4 + 4 * NSPO + 2] = 99; /* bpm */
        old[1 + 4 + 4 * NSPO + 4] = 33; /* cutoff, which GLIDE now sits before */
        old[1 + 4 + 4 * NSPO + NGP_OLD + NLFO * NLP + FX_DMIX] = 66;
        snprintf(path, sizeof path, "%s/old_v3.sav", out);
        FILE *f = fopen(path, "wb");
        fwrite(old, 1, sizeof old, f);
        fclose(f);
        song_default();
        int ok = song_load(path);
        printf("save compat v0.0.1: load=%d on2=%d note2=%d level4=%d bpm=%d cutoff=%d glide=%d dmix=%d string5 level=%d on=%d\n",
               ok, song.on[1], song.sp[1][SP_NOTE], song.sp[3][SP_LEVEL], song.gp[GP_BPM], song.gp[GP_CUTOFF],
               song.gp[GP_GLIDE], song.fx[FX_DMIX], song.sp[4][SP_LEVEL], song.on[4]);
        /* a v0.0.2 file: five strings, seven globals */
        int32_t old4[1 + NS + NS * NSPO + NGP_OLD + NLFO * NLP + 9];
        memset(old4, 0, sizeof old4);
        old4[0] = 0x4a445204; /* JDR4 */
        for (int s = 0; s < NS; s++)
            old4[1 + s] = 1;
        old4[1 + NS + 4 * NSPO + 0] = 9;
        old4[1 + NS + NS * NSPO + 4] = 44; /* cutoff */
        old4[1 + NS + NS * NSPO + 6] = 2;  /* sync LEAD */
        snprintf(path, sizeof path, "%s/old_v4.sav", out);
        f = fopen(path, "wb");
        fwrite(old4, 1, sizeof old4, f);
        fclose(f);
        song_default();
        ok = song_load(path);
        printf("save compat v0.0.2: load=%d note5=%d cutoff=%d sync=%d glide=%d\n", ok, song.sp[4][SP_NOTE],
               song.gp[GP_CUTOFF], song.gp[GP_SYNC], song.gp[GP_GLIDE]);
        /* a v0.0.3 file: PLUCK 1 BAR on string 1 becomes a one-pulse ring of one bar */
        int32_t old5[1 + NS + NS * NSPO + 8 + NLFO * NLP + 9];
        memset(old5, 0, sizeof old5);
        old5[0] = 0x4a445205; /* JDR5 */
        for (int s = 0; s < NS; s++)
            old5[1 + s] = 1;
        old5[1 + NS + 0 * NSPO + 4] = 3; /* PLUCK 1 BAR */
        old5[1 + NS + 2 * NSPO + 0] = 7; /* note */
        old5[1 + NS + NS * NSPO + 1] = 50; /* root */
        old5[1 + NS + NS * NSPO + 4] = 6;  /* glide */
        snprintf(path, sizeof path, "%s/old_v5.sav", out);
        f = fopen(path, "wb");
        fwrite(old5, 1, sizeof old5, f);
        fclose(f);
        song_default();
        ok = song_load(path);
        printf("save compat v0.0.3: load=%d scale=%d root=%d glide=%d note3=%d s1 pluck on=%d E(%d,%d) rate=%d s2 pluck=%d couple=%d body=%d changes=%d\n",
               ok, song.gp[GP_SCALE], song.gp[GP_ROOT], song.gp[GP_GLIDE], song.sp[2][SP_NOTE], song.sp[0][SP_PON],
               song.sp[0][SP_PPULSES], song.sp[0][SP_PSTEPS], song.sp[0][SP_RATE], song.sp[1][SP_PON], song.fx[FX_SYMP],
               song.fx[FX_BODY], song.gp[GP_CHANGES]);
    }

    /* a v0.1 save (JDR6): loads as it was, with nothing new switched on */
    {
        enum { NSP6 = 19, NGP6 = 10, NFX6 = 12 };
        int32_t v6[1 + NS + NS * NSP6 + NGP6 + NLFO * NLP + NFX6];
        song_default();
        memset(v6, 0, sizeof v6);
        v6[0] = 0x4a445206;
        int k = 1;
        for (int s = 0; s < NS; s++)
            v6[k++] = 1;
        for (int s = 0; s < NS; s++)
            for (int p = 0; p < NSP6; p++)
                v6[k++] = song.sp[s][p];
        for (int p = 0; p < NGP6; p++)
            v6[k++] = song.gp[p];
        v6[1 + NS + NS * NSP6 + GP_SCALE] = 6; /* YAMAN */
        k += NLFO * NLP;
        v6[k + FX_BODY] = 77;
        char p6[512];
        snprintf(p6, sizeof p6, "%s/old_v6.sav", out);
        FILE *f6 = fopen(p6, "wb");
        fwrite(v6, 1, sizeof v6, f6);
        fclose(f6);
        song_default();
        int ok = song_load(p6);
        printf("save compat v0.1: load=%d scale=%d body=%d symp=%d evolve=%d pan1=%d pan5=%d\n", ok, song.gp[GP_SCALE],
               song.fx[FX_BODY], song.fx[FX_SCOUNT], song.gp[GP_EVOLVE], song.sp[0][SP_PAN], song.sp[4][SP_PAN]);
    }

    /* tone shapers: each must change the spectrum, not the level much */
    {
        static const int which[5] = {SP_LEVEL, SP_FOLD, SP_STIFF, SP_POS, SP_BUZZ};
        static const char *const nm[5] = {"(none)", "FOLD", "STIFF", "POS", "BUZZ"};
        for (int k = 0; k < 5; k++) {
            song_default();
            engine_init(RATE);
            engine_set_playing(1);
            for (int s = 1; s < NS; s++)
                song.on[s] = 0;
            if (k)
                song.sp[0][which[k]] = 70;
            song.gp[GP_DRIFT] = 0;
            song.gp[GP_CHANGES] = 0;
            song.fx[FX_SYMP] = song.fx[FX_BODY] = 0;
            song.fx[FX_DMIX] = 0;
            song.fx[FX_VSEND] = 0;
            song.lfo[0][LP_DEPTH] = 0;
            engine_render(pcm, RATE * 3);
            engine_render(pcm, RATE);
            /* crude brightness: zero crossings per second */
            int zc = 0;
            for (int i = 1; i < RATE; i++)
                zc += (pcm[2 * i] >= 0) != (pcm[2 * i - 2] >= 0);
            printf("tone %s=70: rms %.0f, zero crossings %d\n", nm[k], rms_of(pcm, 0, RATE), zc);
        }
    }

    /* sympathetic coupling: string 1 plucked alone; string 3 (an octave up,
     * silent: no bow, no plucks) must start to ring through the bridge */
    for (int c = 0; c <= 100; c += 50) {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.gp[GP_DRIFT] = 0;
        song.fx[FX_BODY] = 0;
        song.fx[FX_SYMP] = c;
        for (int s = 0; s < NS; s++) {
            song.sp[s][SP_BOW] = 0;
            song.sp[s][SP_PON] = 0;
            song.sp[s][SP_FINE] = 0;
            song.on[s] = s == 0 || s == 2;
        }
        engine_render(pcm, RATE / 10);
        engine_pluck(0);
        float peak = 0;
        for (int i = 0; i < 100; i++) {
            engine_render(pcm, RATE / 100);
            if (live.meter[2] > peak)
                peak = live.meter[2];
        }
        printf("couple %3d: string 3 (untouched) peak %.4f, string 1 %.3f\n", c, peak, live.meter[0]);
    }

    /* body: each type colours the default drone; level stays in range */
    for (int b = -1; b < NBODY; b++) {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_BODY] = b < 0 ? 0 : 80;
        song.fx[FX_BTYPE] = b < 0 ? 0 : b;
        engine_render(pcm, RATE * 4);
        int zc = 0;
        for (int i = 1; i < RATE * 4; i++)
            zc += (pcm[2 * i] >= 0) != (pcm[2 * i - 2] >= 0);
        printf("body %-8s: rms %.0f, zero crossings/s %d\n", b < 0 ? "OFF" : (const char *[]){"WOOD", "GOURD", "BRONZE", "TUNED", "METAL", "CERAMIC", "GLASS"}[b],
               rms_of(pcm, RATE, RATE * 3), zc / 4);
    }

    /* rings: count plucks and bow openings over 4 bars */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        uint32_t c0 = live.chords;
        int hits[NS][NRING] = {{0}};
        float last[NS][NRING] = {{0}};
        float bmin = 1, bmax = 0;
        int secs4 = (int)(16 * 60.0 / song.gp[GP_BPM] * 100); /* 4 bars in 10 ms blocks */
        for (int i = 0; i < secs4; i++) {
            engine_render(pcm, RATE / 100);
            for (int s = 0; s < NS; s++)
                for (int r = 0; r < NRING; r++) {
                    if (live.flash[s][r] > last[s][r] + 0.3f)
                        hits[s][r]++;
                    last[s][r] = live.flash[s][r];
                }
            if (i > 100) {
                bmin = fminf(bmin, live.bow_env[4]);
                bmax = fmaxf(bmax, live.bow_env[4]);
            }
        }
        printf("rings over 4 bars, plucks per string:");
        for (int s = 0; s < NS; s++)
            printf(" E(%d,%d)/%s=%d", song.sp[s][SP_PPULSES], song.sp[s][SP_PSTEPS],
                   (const char *[]){"16", "8", "4", "2", "1b", "2b"}[song.sp[s][SP_RATE]], hits[s][0]);
        printf("\n  string 5 bow ring E(%d,%d): %d openings, swell %.2f..%.2f; chords changed %u (CHANGES off)\n",
               song.sp[4][SP_BPULSES], song.sp[4][SP_BSTEPS], hits[4][1], bmin, bmax, live.chords - c0);
        /* CHANGES 1 BAR: four bars, four chords (bar 0 is the start) */
        song.gp[GP_CHANGES] = 1;
        c0 = live.chords;
        for (int i = 0; i < secs4; i++)
            engine_render(pcm, RATE / 100);
        printf("  CHANGES 1 BAR: %u chord changes in 4 bars\n", live.chords - c0);
    }

    /* sympathetic strings: each tuning, and how much they add */
    for (int t = -1; t < NSTUNE; t++) {
        static const char *const tn[NSTUNE] = {"SCALE", "CHORD", "CHORD+", "DRONE", "FIFTHS"};
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_SCOUNT] = t < 0 ? 0 : 7;
        song.fx[FX_STUNE] = t < 0 ? 0 : t;
        double r = 0, sm = 0;
        for (int i = 0; i < 6; i++) {
            engine_render(pcm, RATE);
            if (i >= 2)
                r += rms_of(pcm, 0, RATE) / 4, sm += live.symp_meter / 4;
        }
        printf("symp %-6s: rms %.0f, symp peak %.3f, tuned:", t < 0 ? "OFF" : tn[t], r, sm);
        for (int k = 0; t >= 0 && k < 7; k++) {
            char nm[16];
            engine_degree_name(engine_symp_degree(k), nm, sizeof nm);
            printf(" %s", nm);
        }
        printf("\n");
    }
    /* CHORD tuning follows a chord change */
    {
        song_default();
        song.fx[FX_SCOUNT] = 6;
        engine_init(RATE);
        engine_set_playing(1);
        engine_render(pcm, RATE / 10);
        engine_next_chord();
        engine_render(pcm, RATE / 10);
        printf("symp after a change to [%s]:", engine_chord_name());
        for (int k = 0; k < 6; k++) {
            char nm[16];
            engine_degree_name(engine_symp_degree(k), nm, sizeof nm);
            printf(" %s", nm);
        }
        printf("\n");
    }
    /* MOTION 0 vs 100 (root steps), EVOLVE 100 (rhythm nudges per change) */
    for (int mo = 0; mo <= 100; mo += 100) {
        song_default();
        song.gp[GP_MOTION] = mo;
        song.gp[GP_EVOLVE] = 100;
        int before[NS][NSP];
        memcpy(before, song.sp, sizeof before);
        printf("MOTION %3d:", mo);
        for (int i = 0; i < 10; i++) {
            engine_next_chord();
            printf(" %s", engine_chord_name());
        }
        int nudged = 0;
        for (int s = 0; s < NS; s++)
            for (int p = SP_PON; p <= SP_BROT; p++)
                nudged += before[s][p] != song.sp[s][p];
        printf("  (EVOLVE 100: %d ring settings changed)\n", nudged);
    }

    /* progressions: eight changes in each scale */
    for (int sc = 0; sc < engine_nscales(); sc++) {
        song_default();
        engine_set_scale(sc);
        printf("%-10s:", engine_scale_name(sc));
        for (int i = 0; i < 8; i++) {
            engine_next_chord();
            printf(" [%s]", engine_chord_name());
        }
        printf("\n            last voicing:");
        for (int s = 0; s < NS; s++) {
            char nm[16];
            engine_degree_name(song.sp[s][SP_NOTE], nm, sizeof nm);
            printf(" %s", nm);
        }
        printf("\n");
    }

    /* binaural: left and right drift in and out of phase at the beat; count
     * the sign flips of their correlation in 50 ms windows (two a beat).
     * Indices into the beat table: 0.5 Hz, 4 Hz, 8 Hz */
    static const int beat_idx[3] = {4, 12, 21};
    static const double beat_want[3] = {0.5, 4.0, 8.0};
    for (int k = 0; k < 3; k++) {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_SCOUNT] = 0;
        song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        song.fx[FX_BINBEAT] = beat_idx[k];
        song.lfo[0][LP_DEPTH] = 0;
        song.gp[GP_DRIFT] = 0;
        for (int s = 0; s < NS; s++)
            song.on[s] = s == 2, song.sp[s][SP_PON] = 0, song.sp[s][SP_PAN] = 0;
        engine_render(pcm, RATE * 2);
        int secs8 = 8;
        engine_render(pcm, RATE * secs8);
        /* the beat lives below 1 kHz (only that part is shifted): lowpass both ears */
        static float lo[2][RATE * 8];
        float a0 = 0, a1 = 0, b0 = 0, b1 = 0, lk = 1.f - expf(-6.2831853f * 600.f / RATE);
        for (int i = 0; i < RATE * secs8; i++) {
            a0 += (pcm[2 * i] - a0) * lk, a1 += (a0 - a1) * lk;
            b0 += (pcm[2 * i + 1] - b0) * lk, b1 += (b0 - b1) * lk;
            lo[0][i] = a1, lo[1][i] = b1;
        }
        int flips = 0, prev = 0;
        for (int w = 0; w + 2400 <= RATE * secs8; w += 1200) {
            double c = 0;
            for (int i = w; i < w + 2400; i++)
                c += (double)lo[0][i] * lo[1][i];
            int sg = c > 0 ? 1 : -1;
            if (prev && sg != prev)
                flips++;
            prev = sg;
        }
        printf("binaural %.1f Hz: L/R correlation flips %.1f per s (want about %.1f)\n", beat_want[k], flips / (double)secs8,
               beat_want[k] * 2);
    }
    /* OMBAK: both ears beat together (no flips between them), and the low
     * end swells and fades at the beat rate, 4 Hz here */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_SCOUNT] = 0;
        song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        song.fx[FX_BINBEAT] = 12; /* 4 Hz */
        song.fx[FX_BINLOCK] = BIN_OMBAK;
        song.lfo[0][LP_DEPTH] = 0;
        song.gp[GP_DRIFT] = 0;
        for (int s = 0; s < NS; s++)
            song.on[s] = s == 2, song.sp[s][SP_PON] = 0, song.sp[s][SP_PAN] = 0;
        engine_render(pcm, RATE * 2);
        engine_render(pcm, RATE * 4);
        float a0 = 0, a1 = 0, b0 = 0, b1 = 0, lk = 1.f - expf(-6.2831853f * 600.f / RATE);
        static float env[RATE * 4 / 240];
        int flips = 0, prev = 0, ne = 0;
        double c = 0, e = 0;
        for (int i = 0; i < RATE * 4; i++) {
            a0 += (pcm[2 * i] - a0) * lk, a1 += (a0 - a1) * lk;
            b0 += (pcm[2 * i + 1] - b0) * lk, b1 += (b0 - b1) * lk;
            c += (double)a1 * b1;
            e += (double)a1 * a1;
            if ((i + 1) % 240 == 0) { /* 5 ms windows */
                int sg = c > 0 ? 1 : -1;
                if (prev && sg != prev && fabs(c) > 1e3)
                    flips++;
                prev = sg;
                env[ne++] = (float)sqrt(e / 240);
                c = e = 0;
            }
        }
        /* the swell's period: where the envelope best matches itself, 1.3 .. 20 Hz */
        double mean = 0;
        for (int i = 0; i < ne; i++)
            mean += env[i] / ne;
        /* the envelope's self-match falls from lag 0 and rises again at the
         * period: skip to where it first goes below zero, then take the peak */
        static double cc[160];
        for (int lag = 1; lag < 160; lag++) {
            cc[lag] = 0;
            for (int i = 0; i + lag < ne; i++)
                cc[lag] += (env[i] - mean) * (env[i + lag] - mean);
            cc[lag] /= ne - lag;
        }
        int start = 1, best = 0;
        while (start < 159 && cc[start] > 0)
            start++;
        double bestc = -1e30;
        for (int lag = start; lag < 160; lag++)
            if (cc[lag] > bestc)
                bestc = cc[lag], best = lag;
        double swell_hz = best ? RATE / 240.0 / best : 0;
        printf("ombak 4 Hz: L/R flips %d (want 0), swells %.1f per s (want about 4)\n", flips, swell_hz);
    }
    /* the Balinese gong kebyar profiles: each degree where Toth put it */
    {
        static const char *const bal[3] = {"BEGBEG", "SEDANG", "TIRUS"};
        static const float want[3][5] = {{0, 120, 234, 666, 747}, {0, 136, 291, 670, 804}, {0, 197, 377, 724, 828}};
        int ok = 0;
        song_default();
        for (int b = 0; b < 3; b++)
            for (int sc = 0; sc < engine_nscales(); sc++)
                if (!strcmp(engine_scale_name(sc), bal[b])) {
                    engine_set_scale(sc);
                    int good = engine_scale_n() == 5;
                    for (int i = 0; i < 5 && good; i++)
                        good = fabsf(engine_scale_cents(i) - want[b][i]) < 0.5f;
                    ok += good;
                }
        printf("balinese tunings: %d/3 profiles in place\n", ok);
    }
    /* a journey starts from alpha: ALPHA to THETA begins at 10 Hz */
    song_default();
    engine_init(RATE);
    engine_set_playing(1);
    song.fx[FX_BINBEAT] = 16;
    song.fx[FX_BINLOCK] = 2;
    engine_render(pcm, RATE);
    printf("binaural journey A>T: beat now %.2f Hz (starts at 10)\n", live.bin_hz);

    /* HARM: string 1 (A2) touched at 1/2 sounds an octave up: the waveform
     * repeats every half period */
    for (int h = 0; h <= 1; h++) {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_SCOUNT] = 0;
        song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        song.fx[FX_BODY] = 0;
        song.lfo[0][LP_DEPTH] = 0;
        song.gp[GP_DRIFT] = 0;
        for (int s = 0; s < NS; s++)
            song.on[s] = s == 0, song.sp[s][SP_PON] = 0;
        song.sp[0][SP_HARM] = h;
        song.sp[0][SP_BOW] = 60;
        engine_render(pcm, RATE * 3);
        int T = RATE / 110;
        double c0 = 0, ch = 0, cf = 0;
        for (int i = RATE; i < RATE * 2; i++) {
            c0 += (double)pcm[2 * i] * pcm[2 * i];
            ch += (double)pcm[2 * i] * pcm[2 * (i + T / 2)];
            cf += (double)pcm[2 * i] * pcm[2 * (i + T)];
        }
        printf("HARM %s: correlation at half a period %.2f, at a period %.2f\n", h ? "2ND" : "OFF", ch / c0, cf / c0);
    }

    /* chance: string 1 E(4,4) at 1/4 with 50%% chance plucks about half the time */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.sp[0][SP_PPULSES] = 4;
        song.sp[0][SP_PCHANCE] = 50;
        int hits = 0;
        float last = 0;
        int blocks = (int)(64 * 60.0 / song.gp[GP_BPM] * 100); /* 64 beats */
        for (int i = 0; i < blocks; i++) {
            engine_render(pcm, RATE / 100);
            if (live.flash[0][0] > last + 0.3f)
                hits++;
            last = live.flash[0][0];
        }
        printf("chance 50%%: %d plucks of 64 pulses\n", hits);
    }

    /* RELATE: how many notes each chord keeps from the one before */
    for (int rel = 0; rel <= 2; rel++) {
        song_default();
        song.gp[GP_RELATE] = rel;
        int kept = 0, changes = 40;
        for (int i = 0; i < changes; i++) {
            int before[NS], n = engine_scale_n();
            for (int s = 0; s < NS; s++)
                before[s] = ((song.sp[s][SP_NOTE] % n) + n) % n;
            engine_next_chord();
            int common = 0;
            for (int a = 0; a < NS; a++) {
                int pc = ((song.sp[a][SP_NOTE] % n) + n) % n, seen = 0;
                for (int b = 0; b < a; b++)
                    seen |= ((song.sp[b][SP_NOTE] % n) + n) % n == pc;
                if (seen)
                    continue;
                for (int b = 0; b < NS; b++)
                    if (before[b] == pc) {
                        common++;
                        break;
                    }
            }
            kept += common;
        }
        printf("RELATE %d: on average %.2f notes kept per change\n", rel, kept / 40.0);
    }

    /* string GATE: with the bow ring breathing, GATE 100 makes the string's
     * level follow it */
    for (int g = 0; g <= 100; g += 100) {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_SCOUNT] = 0;
        song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        for (int s = 0; s < NS; s++)
            song.on[s] = s == 4, song.sp[s][SP_PON] = 0;
        song.sp[4][SP_BCHANCE] = 100;
        song.sp[4][SP_GATE] = g;
        engine_render(pcm, RATE * 3);
        engine_render(pcm, RATE * 3);
        double lo = 1e9, hi = 0;
        for (int i = 0; i + 2400 <= RATE * 3; i += 2400) {
            double r = rms_of(pcm, i, 2400);
            lo = r < lo ? r : lo;
            hi = r > hi ? r : hi;
        }
        printf("string gate %3d: 50 ms rms ranges %.0f .. %.0f\n", g, lo, hi);
    }

    /* wifi sync: a leader at 128 BPM 5.3 steps ahead; lock tempo and phase */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_SYNC] = SYNC_AUTO;
        sync_core_init(0x80000000u);
        uint8_t pkt[SYNC_PKT_LEN];
        uint64_t t = 0;
        double lpos = 5.3, lbpm = 128.0, err = 0;
        for (int i = 0; i < 400; i++) {
            if (i % 10 == 0) {
                memcpy(pkt, "HI5S", 4);
                pkt[4] = 1, pkt[5] = 1, pkt[6] = SYNC_AUTO, pkt[7] = 0;
                uint32_t id = 7;
                memcpy(pkt + 8, &id, 4);
                float b = (float)lbpm;
                memcpy(pkt + 12, &b, 4);
                double wire = lpos + SYNC_WIRE_LEAD_US * lbpm / 15e6; /* a current unit's position on the wire */
                memcpy(pkt + 16, &wire, 8);
                memcpy(pkt + 24, &t, 8);
                sync_handle_packet(pkt, SYNC_PKT_MIN, t);
            }
            engine_render(pcm, RATE / 100);
            lpos += lbpm / 60.0 * 4.0 * 0.01;
            t += 10000;
            if (i == 399) {
                err = fmod(lpos - engine_position(), 16.0);
                if (err > 8)
                    err -= 16;
                if (err <= -8)
                    err += 16;
            }
        }
        const SyncStatus *ss = sync_status();
        printf("sync: bpm=%.1f peers=%d leader=%d phase err after 4 s = %.3f steps\n", engine_bpm(), ss->peers,
               ss->have_leader, err);
        engine_set_rate(1.0);
    }

    /* wifi sync timing over a slow, uneven network */
    {
        double mean, worst;
        float delay;
        sim_sync(1, &mean, &worst, &delay);
        printf("sync timing: packets 3-43 ms late, read every 33 ms -> off by %.2f ms on average, %.2f at worst, "
               "trip measured %.1f ms (want under 1 and 2: %d)\n",
               mean, worst, delay, mean < 1.0 && worst < 2.0);
        sim_sync(0, &mean, &worst, &delay);
        printf("sync timing, a leader without echo: off by %.2f ms on average (want under 6: %d)\n", mean, mean < 6.0);
    }

    /* pops: in 5 ms windows, the share of energy far above the master
     * lowpass, which only clipping or steps make; and full-scale samples */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        int n = 20 * RATE, worst_i = 0, bad = 0, full = 0;
        double worst = 0;
        int16_t *p = malloc(sizeof(int16_t) * 2 * n);
        for (int i = 0; i < n; i += 1024)
            engine_render(p + 2 * i, n - i < 1024 ? n - i : 1024);
        for (int w = RATE; w + 240 < n; w += 240) {
            double e = 0, h = 0;
            for (int i = w; i < w + 240; i++) {
                double d = p[2 * i] - 3.0 * p[2 * i - 2] + 3.0 * p[2 * i - 4] - p[2 * i - 6];
                e += (double)p[2 * i] * p[2 * i];
                h += d * d;
            }
            if (e < 240 * 1e4)
                continue;
            if (h / e > worst)
                worst = h / e, worst_i = w;
            bad += h / e > 0.05;
        }
        for (int i = 0; i < n; i++)
            full += abs(p[2 * i]) > 31500;
        printf("pops: worst window %.3f at %.2f s (want < 0.5), %d windows over 0.05, %d full-scale samples (want 0)\n", worst,
               (double)worst_i / RATE, bad, full);
        /* mute: string 1 alone (no sympathetic strings), muted, must fade to silence */
        song.fx[FX_SCOUNT] = 0;
        for (int s = 1; s < NS; s++)
            song.on[s] = 0;
        song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        engine_init(RATE); /* no reverb tail left from before */
        engine_render(p, RATE);
        double before = rms_of(p, RATE / 2, RATE / 2);
        song.on[0] = 0;
        engine_render(p, RATE / 2);
        printf("mute: string 1 rms %.0f, 0.4 s after muting %.0f (want 0)\n", before, rms_of(p, RATE * 2 / 5, RATE / 10));
        free(p);
    }

    /* WIND: every exciter in every material, three octaves: in tune within 25 cents */
    {
        static const char *const exn[NEXCITE] = {"LIP", "REED", "DOUBLE", "FLUTE", "FREE", "RECORDER", "BANSURI"};
        for (int ex = 0; ex < NEXCITE; ex++) {
            int ok = 0, tot = 0;
            double worst = 0, lvl = 0;
            for (int mat = 0; mat < NMATERIAL; mat++)
                for (int note = -7; note <= 7; note += 7) {
                    solo(ENG_WIND, ex, mat, note, 50);
                    engine_render(pcm, RATE * 3);
                    engine_render(pcm, RATE);
                    double want = 110.0 * pow(2.0, note / 7.0) * (ex >= EX_FLUTE && ex != EX_FREE ? 2 : 1);
                    double c = 1200 * log2(hz_yin(pcm, RATE / 4, RATE / 4) / want);
                    tot++;
                    ok += fabs(c) < 25;
                    if (fabs(c) >= 25 && getenv("JD_DEBUG"))
                        printf("  off: %s mat %d note %d: %+.0f c\n", exn[ex], mat, note, c);
                    if (fabs(c) > fabs(worst))
                        worst = c;
                    lvl += rms_of(pcm, 0, RATE) / 12;
                }
            printf("wind %-8s: %2d/%d in tune (worst %+.0f cents), rms %.0f\n", exn[ex], ok, tot, worst, lvl);
        }
        /* overblowing by hand (R1 with L1 held): a flute jumps an octave, a reed a twelfth */
        static const int obx[2] = {EX_FLUTE, EX_REED};
        for (int k = 0; k < 2; k++) {
            solo(ENG_WIND, obx[k], MAT_WOOD, 0, 50);
            engine_render(pcm, RATE * 2);
            double f0 = hz_yin(pcm, RATE, RATE / 4);
            engine_bow(2, 1);
            engine_overblow(2, 1);
            engine_render(pcm, RATE * 2);
            double f1 = hz_yin(pcm, RATE, RATE / 4);
            engine_overblow(2, 0);
            engine_bow(2, 0);
            printf("overblow %s: %.1f Hz -> %.1f Hz (x%.2f, want x%d)\n", exn[obx[k]], f0, f1, f1 / f0, k ? 3 : 2);
        }
        /* the same combination on a string touches the 2nd harmonic */
        solo(ENG_STRINGS, 0, 0, 0, 0);
        engine_bow(2, 1);
        engine_render(pcm, RATE * 2);
        double a0 = hz_yin(pcm, RATE, RATE / 4);
        engine_overblow(2, 1);
        engine_render(pcm, RATE * 2);
        double a1 = hz_yin(pcm, RATE, RATE / 4);
        engine_bow(2, 0);
        engine_overblow(2, 0);
        printf("string harmonic by hand: %.1f Hz -> %.1f Hz (want x2)\n", a0, a1);
    }

    /* STRUCK: the pitched models in tune; a drum dies sooner than a gong; a
     * bowed bowl and cymbal sing steadily; a hard mallet brighter than a
     * soft one; a touch (L1 + R1) moves the struck pitch up */
    {
        static const int pitched[7] = {PC_GENDER, PC_GONG, PC_BOWL, PC_HANDPAN, PC_STEELPAN, PC_TONGUE, PC_DRUM};
        int ok = 0;
        double worst = 0;
        for (int k = 0; k < 7; k++) {
            solo(ENG_STRINGS, 0, 0, 0, 0);
            for (int v = 0; v < NS; v++)
                song.sp[v][SP_VOICE] = VOICE_PERC(pitched[k]);
            engine_render(pcm, RATE / 2);
            engine_pluck(2);
            engine_render(pcm, RATE / 2);
            double c = 1200 * log2(hz_yin(pcm, RATE / 10, RATE / 4) / 110.0);
            ok += fabs(c) < 25;
            if (fabs(c) >= 25 && getenv("JD_DEBUG"))
                printf("  off: struck model %d: %+.0f c\n", pitched[k], c);
            if (fabs(c) > fabs(worst))
                worst = c;
        }
        float ring[2];
        static const int rp[2] = {PC_DRUM, PC_GONG};
        for (int k = 0; k < 2; k++) {
            solo(ENG_STRINGS, 0, 0, 0, 0);
            for (int v = 0; v < NS; v++)
                song.sp[v][SP_VOICE] = VOICE_PERC(rp[k]);
            engine_render(pcm, RATE / 2);
            engine_pluck(2);
            engine_render(pcm, RATE * 2 - 2048);
            engine_render(pcm, 2048); /* the meter holds a render's peak: read the last moments */
            ring[k] = live.meter[2];
        }
        float sing[2][2];
        static const int bp[2] = {PC_BOWL, PC_CYMBAL};
        for (int k = 0; k < 2; k++) {
            solo(ENG_STRINGS, 0, 0, 0, 70);
            for (int v = 0; v < NS; v++)
                song.sp[v][SP_VOICE] = VOICE_PERC(bp[k]);
            engine_render(pcm, RATE * 3);
            engine_render(pcm, 2048);
            sing[k][0] = live.meter[2];
            engine_render(pcm, RATE * 3);
            engine_render(pcm, 2048);
            sing[k][1] = live.meter[2];
        }
        double hf[2];
        for (int k = 0; k < 2; k++) {
            solo(ENG_STRINGS, 0, 0, 0, 0);
            for (int v = 0; v < NS; v++)
                song.sp[v][SP_VOICE] = VOICE_PERC(PC_GENDER), song.sp[v][SP_BRIGHT] = k ? 95 : 5;
            engine_render(pcm, RATE / 2);
            engine_pluck(2);
            engine_render(pcm, RATE / 5);
            double e = 0, h = 0;
            for (int i = 600; i < RATE / 10; i++) {
                double d = pcm[2 * i] - 2.0 * pcm[2 * i - 2] + pcm[2 * i - 4];
                e += (double)pcm[2 * i] * pcm[2 * i];
                h += d * d;
            }
            hf[k] = h / (e + 1);
        }
        solo(ENG_STRINGS, 0, 0, 0, 70);
        for (int v = 0; v < NS; v++)
            song.sp[v][SP_VOICE] = VOICE_PERC(PC_BOWL);
        engine_render(pcm, RATE * 3);
        double t0 = hz_yin(pcm, RATE, RATE / 4);
        engine_overblow(2, 1);
        engine_render(pcm, RATE * 3);
        double t1 = hz_yin(pcm, RATE * 2, RATE / 4);
        engine_overblow(2, 0);
        /* a metal drone: RING at the top, the gender still near full 20 s on */
        solo(ENG_STRINGS, 0, 0, 0, 0);
        for (int v = 0; v < NS; v++)
            song.sp[v][SP_VOICE] = VOICE_PERC(PC_GENDER), song.sp[v][SP_DECAY] = 100;
        engine_render(pcm, RATE / 2);
        engine_pluck(2);
        engine_render(pcm, RATE - 2048);
        engine_render(pcm, 2048);
        float d1 = live.meter[2];
        for (int i = 0; i < 19; i++)
            engine_render(pcm, RATE);
        engine_render(pcm, 2048);
        float d20 = live.meter[2];
        /* the unearthly bodies: each sounds, stays finite, CHORD and ALIEN at their note */
        int alive = 0;
        double fc = 0, fa = 0;
        for (int k = PC_GLASS; k <= PC_ALIEN; k++) {
            solo(ENG_STRINGS, 0, 0, 0, 0);
            for (int v = 0; v < NS; v++)
                song.sp[v][SP_VOICE] = VOICE_PERC(k);
            engine_render(pcm, RATE / 2);
            engine_pluck(2);
            engine_render(pcm, RATE);
            double r = rms_of(pcm, RATE / 10, RATE / 4);
            alive += r > 200 && r == r;
            if (k == PC_CHORD)
                fc = hz_yin(pcm, RATE / 10, RATE / 4);
            if (k == PC_ALIEN)
                fa = hz_yin(pcm, RATE / 10, RATE / 4);
        }
        /* the body's limiter: strike after strike on a droning bowl levels off
         * (without it, ten minutes of ring would let forty strikes pile up) */
        solo(ENG_STRINGS, 0, 0, 0, 0);
        for (int v = 0; v < NS; v++)
            song.sp[v][SP_VOICE] = VOICE_PERC(PC_BOWL), song.sp[v][SP_DECAY] = 100;
        float early = 0, late = 0;
        for (int i = 0; i < 40; i++) {
            engine_pluck(2);
            engine_render(pcm, RATE / 8);
            if (i >= 5 && i < 10)
                early = fmaxf(early, live.meter[2]);
            if (i >= 30)
                late = fmaxf(late, live.meter[2]);
        }
        printf("struck limiter: strikes 5-10 reach %.2f, strikes 30-40 %.2f (want it levelled off: %d)\n", early, late,
               late < 1.25f * early);
        printf("metal drone: RING DRONE, level after 1 s %.3f, after 20 s %.3f (want over 70%%: %d); beyond: %d/3 sound, "
               "CHORD %.1f Hz, ALIEN %.1f Hz\n", d1, d20, d20 > 0.7f * d1, alive, fc, fa);
        printf("struck: %d/7 pitched models in tune (worst %+.0f cents); ring after 2 s drum %.3f, gong %.3f; bowed bowl %.3f -> %.3f, "
               "cymbal %.3f -> %.3f; mallet soft %.3g hard %.3g; bowl touched %.0f -> %.0f Hz\n",
               ok, worst, ring[0], ring[1], sing[0][0], sing[0][1], sing[1][0], sing[1][1], hf[0], hf[1], t0, t1);
        printf("struck checks: tune %d, drum shorter %d, bowl sings %d, cymbal sings %d, hard brighter %d, touch higher %d\n",
               ok == 7, ring[0] < 0.3f * ring[1], sing[0][1] > 0.01f && sing[0][1] > 0.6f * sing[0][0],
               sing[1][1] > 0.01f && sing[1][1] > 0.6f * sing[1][0], hf[1] > hf[0], t1 > 1.5 * t0);
    }

    /* SOUND: R2 + X on a VOICE jumps a family at a time: string -> the first
     * wind -> the first struck voice -> string */
    {
        song_default();
        engine_init(RATE);
        next_page(); /* PLAY -> SOUND */
        for (int i = 0; i < 12; i++)
            press(BTN_UP); /* to the top row: VOICE */
        int seq[3] = {-1, -1, -1}, col = -1;
        for (int k = 0; k < 3; k++) {
            int before[NS];
            for (int v = 0; v < NS; v++)
                before[v] = song.sp[v][SP_VOICE];
            ui_button(BTN_R2, 1);
            press(BTN_X);
            ui_button(BTN_R2, 0);
            for (int v = 0; v < NS; v++)
                if (song.sp[v][SP_VOICE] != before[v])
                    col = v;
            seq[k] = col >= 0 ? song.sp[col][SP_VOICE] : -1;
            ui_tick(50);
        }
        for (int i = 0; i < PAGE_COUNT - 1; i++)
            next_page(); /* back to PLAY */
        printf("family jump: %s -> %s -> %s\n", seq[0] >= 0 ? engine_voice_name(seq[0]) : "?",
               seq[1] >= 0 ? engine_voice_name(seq[1]) : "?", seq[2] >= 0 ? engine_voice_name(seq[2]) : "?");
    }

    /* undertones: energy an octave and a twelfth below a 110 Hz voice */
    for (int eng = 0; eng < NENGINE; eng++)
        for (int sub = 0; sub <= 70; sub += 70) {
            solo(eng, EX_REED, MAT_WOOD, 0, 60);
            song.gp[GP_SUB] = sub;
            song.gp[GP_SUBDIV] = SUB_OCT_TWELFTH;
            engine_render(pcm, RATE * 3);
            engine_render(pcm, RATE);
            double e[3];
            static const double fq[3] = {110.0, 55.0, 110.0 / 3.0};
            for (int k = 0; k < 3; k++) {
                double w = 2 * M_PI * fq[k] / RATE, c = 2 * cos(w), s1 = 0, s2 = 0;
                for (int i = 0; i < RATE; i++) {
                    double s0 = pcm[2 * i] + c * s1 - s2;
                    s2 = s1;
                    s1 = s0;
                }
                e[k] = sqrt(s1 * s1 + s2 * s2 - c * s1 * s2) / RATE;
            }
            printf("undertones %s SUB %2d: 110 Hz %.0f, 55 Hz %.0f, 36.7 Hz %.0f\n", eng ? "WIND   " : "STRINGS", sub, e[0], e[1], e[2]);
        }

    /* presets: save YAMAN to slot 3, change, load it back, peek, clear */
    {
        char p3[512];
        Song t;
        snprintf(p3, sizeof p3, "%s/preset_03.jds", out);
        song_default();
        engine_set_scale(6);
        song_save(p3);
        engine_set_scale(11);
        int before = song.gp[GP_SCALE];
        int ok = song_load(p3);
        int peek = song_peek(p3, &t);
        printf("presets: saved YAMAN, changed to %s, loaded back -> %s (load %d, peek %d scale %d)\n", engine_scale_name(before),
               engine_scale_name(song.gp[GP_SCALE]), ok, peek, t.gp[GP_SCALE]);
    }
    /* mixed voices: one voice turned from a string into a reed while it
     * plays stays on its note; a v0.5 WIND song gives every voice its exciter */
    {
        solo(ENG_STRINGS, 0, 0, 0, 0);
        song.sp[2][SP_BOW] = 60;
        engine_render(pcm, RATE * 2);
        double fs = hz_yin(pcm, RATE, RATE / 4);
        song.sp[2][SP_VOICE] = 1 + EX_REED;
        engine_render(pcm, RATE * 3);
        double fw = hz_yin(pcm, RATE, RATE / 4), pk = 0;
        for (int i = 0; i < RATE * 3 * 2; i++)
            pk = fmax(pk, abs(pcm[i]) / 32768.0);
        song_default();
        for (int v = 0; v < NS; v++)
            song.on[v] = 1, song.sp[v][SP_VOICE] = v < 2 ? 0 : 1 + v; /* 2 strings, flute, free, recorder */
        engine_init(RATE);
        engine_set_playing(1);
        engine_render(pcm, RATE * 4);
        double rms = 0;
        for (int i = 0; i < RATE * 4 * 2; i++)
            rms += (double)pcm[i] * pcm[i];
        rms = sqrt(rms / (RATE * 8)) / 32768.0;
        struct {
            uint32_t magic;
            int on[NS], sp[NS][24], gp[27], lfo[NLFO][NLP], fx[17]; /* 17 effect settings before AIR */
            int prog_n, prog_pos, prog_note[NPROG][NS], prog_fine[NPROG][NS];
            char prog_name[NPROG][24];
        } old;
        memset(&old, 0, sizeof old);
        old.magic = 0x4a44520bu; /* JDR11 (v0.5) */
        for (int v = 0; v < NS; v++)
            old.on[v] = 1, old.sp[v][SP_LEVEL] = 50, old.sp[v][SP_PSTEPS] = old.sp[v][SP_PPULSES] = 4;
        old.gp[GP_SCALE] = 1, old.gp[GP_ROOT] = 45, old.gp[GP_BPM] = 80;
        old.gp[GP_ENGINE] = ENG_WIND, old.gp[GP_EXCITE] = EX_DOUBLE, old.gp[GP_MATERIAL] = MAT_GLASS;
        char po[512];
        snprintf(po, sizeof po, "%s/old_wind.jds", out);
        FILE *f = fopen(po, "wb");
        fwrite(&old, sizeof old, 1, f);
        fclose(f);
        int ok = song_load(po);
        printf("mixed voices: string %.1f Hz -> reed %.1f Hz (peak %.2f); 2 strings + 3 winds rms %.3f; v0.5 WIND song load %d"
               " voices %s %s body %d (want GLASS %d)\n",
               fs, fw, pk, rms, ok, engine_voice_name(song.sp[0][SP_VOICE]), engine_voice_name(song.sp[4][SP_VOICE]),
               song.fx[FX_BTYPE], BODY_GLASS);
    }
    /* LATCH: silent until played; a pluck starts at once and keeps droning,
     * a bow swells in, played again it fades over its RELEASE (1.3 s) */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_PLAYMODE] = PLAY_LATCH;
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        song.fx[FX_SCOUNT] = 0;
        for (int v = 0; v < NS; v++)
            song.sp[v][SP_RELEASE] = 55;
        engine_render(pcm, RATE * 2);
        double quiet = rms_of(pcm, RATE, RATE);
        engine_latch(1, LATCH_PLUCK);
        engine_render(pcm, RATE / 20);
        double pk_start = rms_of(pcm, 0, RATE / 20);
        engine_render(pcm, RATE * 6);
        double held = rms_of(pcm, RATE * 5, RATE);
        engine_latch(1, LATCH_PLUCK); /* again: let go */
        engine_render(pcm, RATE * 2);
        double gone = rms_of(pcm, RATE + RATE / 2, RATE / 2);
        engine_latch(3, LATCH_BOW);
        engine_render(pcm, RATE * 3);
        double bow_early = rms_of(pcm, 0, RATE / 10), bow_full = rms_of(pcm, RATE * 2, RATE);
        engine_unlatch_all();
        printf("latch: silent %.0f, pluck at once %.0f, still droning after 6 s %.0f, 1.5 s after letting go %.0f; "
               "bow first 0.1 s %.0f, after 2 s %.0f\n",
               quiet, pk_start, held, gone, bow_early, bow_full);
        song.sp[2][SP_VOICE] = 1 + EX_REED; /* a wind voice latches the same way */
        engine_render(pcm, RATE);
        engine_latch(2, LATCH_PLUCK);
        engine_render(pcm, RATE * 4);
        double w_held = rms_of(pcm, RATE * 3, RATE);
        engine_latch(2, LATCH_BOW);
        engine_render(pcm, RATE * 2);
        double w_gone = rms_of(pcm, RATE + RATE / 2, RATE / 2);
        printf("latch checks: silent %d, starts %d, drones %d, fades %d, bow swells %d, wind drones %d and fades %d\n",
               quiet < 30, pk_start > 300, held > 300, gone < held * 0.1, bow_early < bow_full * 0.5, w_held > 300,
               w_gone < w_held * 0.1);
    }
    /* LATCH: a long RELEASE (12 s) dies away instead of being lifted back up
     * by the leveller */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_PLAYMODE] = PLAY_LATCH;
        song.gp[GP_CHANGES] = 0;
        song.fx[FX_SCOUNT] = 0;
        song.sp[1][SP_RELEASE] = 91; /* about 12 s */
        engine_latch(1, LATCH_BOW);
        engine_render(pcm, RATE * 5);
        engine_latch(1, LATCH_BOW); /* let go */
        double lv[3];
        for (int k = 0; k < 3; k++) { /* 1, 5 and 9 s after letting go */
            engine_render(pcm, RATE * (k ? 4 : 1));
            lv[k] = rms_of(pcm, RATE * (k ? 3 : 0), RATE / 2);
        }
        printf("latch release 12 s: level 1 s after %.0f, 5 s %.0f, 9 s %.0f (want it dying away: 9 s under 3%% of 1 s: "
               "%d)\n", lv[0], lv[1], lv[2], lv[2] < 0.03 * lv[0] && lv[1] < lv[0]);
    }
    /* LATCH: the bow ATTACK sets how fast a bowed string swells in, and B
     * (held) sounds the next harmonic of a droning string */
    {
        double early[2];
        static const int atk[2] = {20, 90}; /* about 0.15 s and 6 s */
        for (int k = 0; k < 2; k++) {
            song_default();
            engine_init(RATE);
            engine_set_playing(1);
            song.gp[GP_PLAYMODE] = PLAY_LATCH;
            song.gp[GP_CHANGES] = 0;
            song.fx[FX_SCOUNT] = 0;
            song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
            song.sp[2][SP_LATK] = atk[k];
            song.gp[GP_DRIFT] = 0, song.lfo[0][LP_DEPTH] = 0;
            for (int v = 0; v < NS; v++)
                song.on[v] = v == 2;
            engine_render(pcm, RATE / 2);
            engine_latch(2, LATCH_BOW);
            engine_render(pcm, RATE / 2);
            early[k] = rms_of(pcm, RATE / 4, RATE / 4);
        }
        engine_render(pcm, RATE * 6); /* the long one, full now */
        song.sp[2][SP_LATK] = 40;
        engine_render(pcm, RATE * 2);
        double f0 = hz_yin(pcm, RATE, RATE / 4);
        engine_overblow(2, 1); /* B held */
        engine_render(pcm, RATE * 2);
        double f1 = hz_yin(pcm, RATE, RATE / 4);
        engine_overblow(2, 0);
        engine_render(pcm, RATE * 2);
        double f2 = hz_yin(pcm, RATE, RATE / 4);
        engine_unlatch_all();
        printf("latch attack: 0.25 .. 0.5 s after bowing on, short attack %.0f, long attack %.0f (want short > 3x long: %d)\n",
               early[0], early[1], early[0] > 3 * early[1]);
        printf("latch harmonic: %.1f Hz, B held %.1f Hz (x%.2f, want x2), let go %.1f Hz\n", f0, f1, f1 / f0, f2);
    }
    /* on PLAY the shoulders act at once: L2 plucks, START + L2 drops the dulcimer
     * mallet (no waiting to tell a tap from a hold); the strike is brighter
     * than a pluck; the dropped mallet lands again, lower and sooner; a wind's
     * mallet is a flutter tongue */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_PLAYMODE] = PLAY_LATCH;
        song.gp[GP_CHANGES] = 0;
        int16_t buf[2 * 480];
        engine_render(buf, 480);
        ui_tick(400);
        ui_button(BTN_L2, 1); /* L2: plucked on at once, no tick needed */
        int sel = -1;
        for (int v = 0; v < NS; v++)
            if (live.latch[v])
                sel = v;
        int l1_state = sel >= 0 ? live.latch[sel] : -1;
        ui_button(BTN_L2, 0);
        if (sel >= 0)
            engine_latch(sel, LATCH_PLUCK); /* let it go */
        ui_tick(400);
        ui_button(BTN_START, 1);
        ui_button(BTN_L2, 1); /* START + L2: the mallet, at once */
        int l2_state = sel >= 0 ? live.latch[sel] : -1;
        ui_button(BTN_L2, 0);
        ui_button(BTN_START, 0);
        engine_unlatch_all();
        ui_tick(400);
        song.gp[GP_PLAYMODE] = PLAY_RINGS;
        /* brightness of the ringing string just after: a strike against a pluck */
        double hf[2];
        for (int k = 0; k < 2; k++) {
            solo(ENG_STRINGS, 0, 0, 0, 0);
            engine_render(pcm, RATE / 2);
            if (k)
                engine_hammer(2);
            else
                engine_pluck(2);
            engine_render(pcm, RATE / 10);
            double e = 0, h = 0;
            for (int i = RATE * 3 / 100; i < RATE * 8 / 100; i++) { /* 30 .. 80 ms: the string ringing */
                double d = pcm[2 * i] - 2.0 * pcm[2 * i - 2] + pcm[2 * i - 4];
                e += (double)pcm[2 * i] * pcm[2 * i];
                h += d * d;
            }
            hf[k] = h / (e + 1);
        }
        /* the dropped mallet lands again after the strike, on a string and on a struck body */
        int lands_k[2];
        for (int k = 0; k < 2; k++) {
            solo(ENG_STRINGS, 0, 0, 0, 0);
            if (k)
                for (int v = 0; v < NS; v++)
                    song.sp[v][SP_VOICE] = VOICE_PERC(PC_GENDER);
            engine_render(pcm, RATE / 2);
            engine_mallet_lands = 0;
            engine_hammer(2);
            engine_render(pcm, RATE / 2);
            lands_k[k] = engine_mallet_lands;
        }
        int lands = lands_k[0] < lands_k[1] ? lands_k[0] : lands_k[1];
        /* a flute's mallet: a flutter tongue, the breath cut about 20-26 times a second */
        double flut[2];
        for (int k = 0; k < 2; k++) {
            solo(ENG_WIND, EX_FLUTE, MAT_WOOD, 0, 40);
            engine_render(pcm, RATE * 2);
            if (k)
                engine_hammer(2);
            else
                engine_pluck(2);
            engine_render(pcm, RATE / 2);
            static double e[250];
            double mu = 0;
            for (int w = 0; w < 200; w++) { /* 2 ms windows over the first 400 ms */
                double s2 = 0;
                for (int q = 0; q < 96; q++) {
                    double x = pcm[2 * (w * 96 + q)];
                    s2 += x * x;
                }
                e[w] = sqrt(s2 / 96);
                mu += e[w] / 200;
            }
            double best = 0;
            for (double f = 16; f <= 28; f += 0.5) {
                double re = 0, im = 0;
                for (int w = 0; w < 200; w++)
                    re += (e[w] - mu) * cos(2 * M_PI * f * w * 0.002), im += (e[w] - mu) * sin(2 * M_PI * f * w * 0.002);
                double m = sqrt(re * re + im * im) / 200 / (mu + 1);
                if (m > best)
                    best = m;
            }
            flut[k] = best;
        }
        printf("buttons: L2 -> %s, START + L2 -> %s (want PLUCK, STRIKE, both at once)\n",
               l1_state == LATCH_PLUCK ? "PLUCK" : l1_state == LATCH_HAMMER ? "STRIKE" : "none",
               l2_state == LATCH_HAMMER ? "STRIKE" : l2_state == LATCH_PLUCK ? "PLUCK" : "none");
        printf("strike: brightness %.3f against a pluck's %.3f (want brighter: %d)\n", hf[1], hf[0], hf[1] > hf[0]);
        printf("mallet drop: string %d landings, struck %d (want 3 or more: %d); flute flutter %.3f against a tongued note's %.3f "
               "(want a flutter: %d)\n", lands_k[0], lands_k[1], lands >= 3, flut[1], flut[0], flut[1] > 3 * flut[0] && flut[1] > 0.05);
    }
    /* a v0.6 / v0.7 song (JDR13) loads: its voices kept, RELEASE and PLAY MODE new */
    {
        struct {
            uint32_t magic;
            int on[NS], sp[NS][25], gp[27], lfo[NLFO][NLP], fx[17]; /* 17 effect settings before AIR */
            int prog_n, prog_pos, prog_note[NPROG][NS], prog_fine[NPROG][NS];
            char prog_name[NPROG][24];
        } o13;
        memset(&o13, 0, sizeof o13);
        o13.magic = 0x4a44520du;
        for (int v = 0; v < NS; v++)
            o13.on[v] = 1, o13.sp[v][SP_LEVEL] = 60, o13.sp[v][SP_PSTEPS] = o13.sp[v][SP_PPULSES] = 4,
            o13.sp[v][SP_VOICE] = v;
        o13.gp[GP_SCALE] = 15, o13.gp[GP_ROOT] = 45, o13.gp[GP_BPM] = 66;
        char p13[512];
        snprintf(p13, sizeof p13, "%s/v07.jds", out);
        FILE *f = fopen(p13, "wb");
        fwrite(&o13, sizeof o13, 1, f);
        fclose(f);
        int ok = song_load(p13);
        printf("save compat v0.7: load=%d scale=%s voice5=%s release=%d playmode=%d bpm=%d\n", ok,
               engine_scale_name(song.gp[GP_SCALE]), engine_voice_name(song.sp[4][SP_VOICE]), song.sp[4][SP_RELEASE],
               song.gp[GP_PLAYMODE], song.gp[GP_BPM]);
    }
    /* switching a voice while it sounds: the old voice fades out before the
     * new one starts, so the change makes no step bigger than playing does
     * (averaged over several moments: a bowed string's level wavers) */
    {
        double sum = 0;
        for (int k = 0; k < 6; k++) {
            song_default();
            engine_init(RATE);
            engine_set_playing(1);
            song.gp[GP_CHANGES] = 0;
            song.fx[FX_DMIX] = song.fx[FX_VSEND] = song.fx[FX_SCOUNT] = song.fx[FX_BODY] = 0;
            for (int v = 0; v < NS; v++)
                song.on[v] = v == 2, song.sp[v][SP_PON] = song.sp[v][SP_BON] = 0;
            song.sp[2][SP_BOW] = 70;
            engine_render(pcm, RATE * 3 + k * 1777);
            /* a cut leaves the moments after the change nearly silent; the fade
             * still holds most of the sound 8 ms later */
            engine_render(pcm, RATE / 2);
            double before = rms_of(pcm, RATE / 2 - 480, 480);
            song.sp[2][SP_VOICE] = 1 + EX_REED; /* string -> reed while it plays */
            engine_render(pcm, RATE / 4);
            sum += rms_of(pcm, 384, 240) / before; /* 8 .. 13 ms: past the limiter's 6 ms look-ahead */
        }
        printf("voice switch: %.0f%% of the sound still there 8 ms after the change (want over 75%%: a fade, not a "
               "cut)\n", 100.0 * sum / 6);
    }
    /* both shoulders, either order, sound the harmonic (or overblow) of the
     * selected string: in LATCH over its drone, in RINGS over the hand bow */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_PLAYMODE] = PLAY_LATCH;
        song.gp[GP_CHANGES] = 0, song.gp[GP_DRIFT] = 0, song.lfo[0][LP_DEPTH] = 0;
        song.fx[FX_SCOUNT] = 0, song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        int16_t buf[2 * 480];
        ui_tick(300); /* well after any earlier L2 (a press within 30 ms of a release is switch bounce) */
        ui_button(BTN_R2, 1); /* R2 down: the selected string bows on */
        engine_render(buf, 480);
        int sel = -1;
        for (int v = 0; v < NS; v++)
            if (live.latch[v])
                sel = v;
        for (int v = 0; v < NS; v++) /* only the selected string sounds, for a clean pitch */
            song.on[v] = v == sel;
        engine_render(pcm, RATE * 3);
        double f0 = hz_yin(pcm, RATE * 2, RATE / 4);
        ui_tick(300);
        ui_button(BTN_L2, 1); /* add L2 */
        engine_render(pcm, RATE * 2);
        double f1 = hz_yin(pcm, RATE, RATE / 4);
        ui_button(BTN_L2, 0);
        ui_button(BTN_R2, 0);
        engine_render(pcm, RATE * 2);
        double f2 = hz_yin(pcm, RATE, RATE / 4);
        int still = sel >= 0 && live.latch[sel] == LATCH_BOW;
        ui_tick(300);
        ui_button(BTN_L2, 1); /* L2 first (held: no tap), then R2 */
        ui_tick(200);
        ui_button(BTN_R2, 1);
        engine_render(pcm, RATE * 2);
        double f3 = hz_yin(pcm, RATE, RATE / 4);
        ui_button(BTN_R2, 0);
        ui_button(BTN_L2, 0);
        engine_unlatch_all();
        song.gp[GP_PLAYMODE] = PLAY_RINGS;
        /* RINGS: hold R2 (a flute blown by hand), then add L2: it overblows */
        for (int v = 0; v < NS; v++)
            song.sp[v][SP_VOICE] = 1 + EX_FLUTE, song.sp[v][SP_PON] = song.sp[v][SP_BON] = 0, song.sp[v][SP_BOW] = 0;
        engine_render(pcm, RATE);
        ui_tick(300);
        ui_button(BTN_R2, 1);
        engine_render(pcm, RATE * 2);
        ui_tick(300);
        double w0 = hz_yin(pcm, RATE, RATE / 4);
        ui_button(BTN_L2, 1);
        engine_render(pcm, RATE * 2);
        double w1 = hz_yin(pcm, RATE, RATE / 4);
        ui_button(BTN_L2, 0);
        ui_button(BTN_R2, 0);
        ui_tick(300);
        printf("both shoulders: LATCH R2 then L2 %.1f -> %.1f Hz (x%.2f), let go %.1f Hz, still droning %d; L2 then R2 x%.2f; "
               "RINGS R2 then L2 %.1f -> %.1f Hz (x%.2f)\n",
               f0, f1, f1 / f0, f2, still, f3 / f0, w0, w1, w1 / w0);
    }
    /* LATCH: R2 bows on as it is pressed; held for an edit (R2 + right: the
     * next scale), the string goes back as it was; L1 / R1 change the page */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_PLAYMODE] = PLAY_LATCH;
        ui_tick(300);
        int sel = ui_selected_string(), sc = song.gp[GP_SCALE];
        ui_button(BTN_R2, 1);
        int on_press = live.latch[sel] == LATCH_BOW;
        press(BTN_RIGHT);
        int undone = live.latch[sel] == LATCH_OFF && song.gp[GP_SCALE] != sc;
        ui_button(BTN_R2, 0);
        press(BTN_R2); /* a plain tap: it drones */
        int tap = live.latch[sel] == LATCH_BOW;
        press(BTN_R2); /* and again: let go */
        int again = live.latch[sel] == LATCH_OFF;
        engine_unlatch_all();
        song.gp[GP_PLAYMODE] = PLAY_RINGS;
        int pg = ui_page();
        press(BTN_R1);
        int next = ui_page();
        press(BTN_L1);
        int back = ui_page();
        /* only START (or a clock) starts the drone: playing by hand with it
         * stopped sounds, and leaves it stopped */
        song_default();
        song.gp[GP_CHANGES] = 0;
        engine_init(RATE);
        engine_set_playing(0);
        engine_render(pcm, RATE / 2);
        double quiet0 = rms_of(pcm, 0, RATE / 2);
        ui_tick(300);
        press(BTN_L2); /* a pluck */
        engine_render(pcm, RATE / 2);
        double pl = rms_of(pcm, 0, RATE / 4);
        int still_stopped = !live.playing;
        song.gp[GP_PLAYMODE] = PLAY_LATCH;
        ui_tick(300);
        press(BTN_R2); /* LATCH: bowed on, droning */
        engine_render(pcm, RATE * 2);
        double dr = rms_of(pcm, RATE, RATE / 2);
        still_stopped &= !live.playing;
        engine_unlatch_all();
        song.gp[GP_PLAYMODE] = PLAY_RINGS;
        engine_render(pcm, RATE * 3);
        /* with the drone playing, a START tap stops it and lets the latched string go */
        song.gp[GP_PLAYMODE] = PLAY_LATCH;
        engine_set_playing(1);
        ui_tick(300);
        press(BTN_R2);
        int was_latched = live.latch[ui_selected_string()] != LATCH_OFF;
        press(BTN_START);
        int start_lets_go = was_latched && live.latch[ui_selected_string()] == LATCH_OFF && !live.playing;
        song.gp[GP_PLAYMODE] = PLAY_RINGS;
        engine_render(pcm, RATE * 3);
        int stopped_ok = quiet0 < 1.0 && pl > 100.0 && dr > 100.0 && still_stopped && start_lets_go;
        /* RINGS: R2 held bows; R2 + up (TONE) is an edit: the bow lifts while R2 is still held */
        engine_set_playing(1);
        ui_tick(300);
        ui_button(BTN_R2, 1);
        engine_render(pcm, RATE);
        float bowing_env = live.hand_env[ui_selected_string()];
        press(BTN_UP);
        engine_render(pcm, RATE);
        float lifted_env = live.hand_env[ui_selected_string()];
        ui_button(BTN_R2, 0);
        int combo_ok = bowing_env > 0.2f && lifted_env < 0.02f;
        engine_set_playing(0);
        printf("stopped: silent %.0f, L2 plucks %.0f, LATCH R2 drones %.0f, START stops the latch %d, the drone still "
               "stopped (1: %d); R2 + up lifts the bow %.2f -> %.2f (1: %d)\n",
               quiet0, pl, dr, start_lets_go, stopped_ok, bowing_env, lifted_env, combo_ok);
        printf("shoulders: LATCH R2 bows at once %d, used for an edit put back %d, a tap drones %d, again lets go %d; "
               "R1 page %d -> %d, L1 back %d: %d\n",
               on_press, undone, tap, again, pg, next, back,
               on_press && undone && tap && again && next == (pg + 1) % PAGE_COUNT && back == pg);
        song_default();
    }
    /* a v0.8 song (JDR14) loads: LATCH and RELEASE kept, the bow ATTACK new */
    {
        struct {
            uint32_t magic;
            int on[NS], sp[NS][26], gp[28], lfo[NLFO][NLP], fx[17]; /* 17 effect settings before AIR */
            int prog_n, prog_pos, prog_note[NPROG][NS], prog_fine[NPROG][NS];
            char prog_name[NPROG][24];
        } o14;
        memset(&o14, 0, sizeof o14);
        o14.magic = 0x4a44520eu;
        for (int v = 0; v < NS; v++)
            o14.on[v] = 1, o14.sp[v][SP_LEVEL] = 60, o14.sp[v][SP_PSTEPS] = o14.sp[v][SP_PPULSES] = 4,
            o14.sp[v][SP_RELEASE] = 80;
        o14.gp[GP_SCALE] = 16, o14.gp[GP_ROOT] = 45, o14.gp[GP_BPM] = 70, o14.gp[GP_PLAYMODE] = PLAY_LATCH;
        char p14[512];
        snprintf(p14, sizeof p14, "%s/v08.jds", out);
        FILE *f = fopen(p14, "wb");
        fwrite(&o14, sizeof o14, 1, f);
        fclose(f);
        int ok = song_load(p14);
        printf("save compat v0.8: load=%d scale=%s playmode=%d release=%d attack=%d\n", ok,
               engine_scale_name(song.gp[GP_SCALE]), song.gp[GP_PLAYMODE], song.sp[0][SP_RELEASE], song.sp[0][SP_LATK]);
    }
    /* a v0.9-0.11 song (JDR15) loads: everything kept, AIR off, every voice playing */
    {
        struct {
            uint32_t magic;
            int on[NS], sp[NS][27], gp[28], lfo[NLFO][NLP], fx[17];
            int prog_n, prog_pos, prog_note[NPROG][NS], prog_fine[NPROG][NS];
            char prog_name[NPROG][24];
        } o15;
        memset(&o15, 0, sizeof o15);
        o15.magic = 0x4a44520fu;
        for (int v = 0; v < NS; v++)
            o15.on[v] = 1, o15.sp[v][SP_LEVEL] = 60, o15.sp[v][SP_PSTEPS] = o15.sp[v][SP_PPULSES] = 4,
            o15.sp[v][SP_LATK] = 42;
        o15.gp[GP_SCALE] = 16, o15.gp[GP_ROOT] = 45, o15.gp[GP_BPM] = 70;
        o15.fx[FX_SRING] = 77;
        char p15[512];
        snprintf(p15, sizeof p15, "%s/v011.jds", out);
        FILE *f = fopen(p15, "wb");
        fwrite(&o15, sizeof o15, 1, f);
        fclose(f);
        int ok = song_load(p15), listening = 0;
        for (int v = 0; v < NS; v++)
            listening += song.sp[v][SP_LISTEN] == AIR_LISTENS;
        printf("save compat v0.11: load=%d attack=%d s ring=%d air=%d listening=%d\n", ok, song.sp[0][SP_LATK],
               song.fx[FX_SRING], song.fx[FX_AIR], listening);
    }
    /* a v0.12 song (JDR16) loads: everything kept, stereo out */
    {
        struct {
            uint32_t magic;
            int on[NS], sp[NS][NSP], gp[28], lfo[NLFO][NLP], fx[NFX];
            int prog_n, prog_pos, prog_note[NPROG][NS], prog_fine[NPROG][NS];
            char prog_name[NPROG][24];
        } o16;
        memset(&o16, 0, sizeof o16);
        o16.magic = 0x4a445210u;
        for (int v = 0; v < NS; v++)
            o16.on[v] = 1, o16.sp[v][SP_LEVEL] = 60, o16.sp[v][SP_PSTEPS] = o16.sp[v][SP_PPULSES] = 4;
        o16.gp[GP_SCALE] = 16, o16.gp[GP_ROOT] = 45, o16.gp[GP_BPM] = 70, o16.gp[GP_PLAYMODE] = PLAY_LATCH;
        o16.fx[FX_AIR] = 33;
        char p16[512];
        snprintf(p16, sizeof p16, "%s/v012.jds", out);
        FILE *f = fopen(p16, "wb");
        fwrite(&o16, sizeof o16, 1, f);
        fclose(f);
        int ok = song_load(p16);
        printf("save compat v0.12: load=%d bpm=%d playmode=%d air=%d output=%d\n", ok, song.gp[GP_BPM],
               song.gp[GP_PLAYMODE], song.fx[FX_AIR], song.gp[GP_OUTPUT]);
    }
    /* mono + clock out: the pulses' count, spacing and width on the clock
     * side, nothing but pulses there, and the sound on the other side */
    {
        static const int idx[3] = {1, 5, 6}; /* 2, 24 and 32 PPQN */
        for (int k = 0; k < 4; k++) {
            song_default();
            for (int v = 0; v < NS; v++)
                song.sp[v][SP_PAN] = v & 1 ? 50 : -50; /* hard pans: played from the centre */
            song.gp[GP_OUTPUT] = OUT_MONO_CLOCK;
            song.gp[GP_PPQN] = idx[k < 3 ? k : 0];
            song.gp[GP_CLKSIDE] = k == 3; /* the last pass: clock on the left */
            engine_init(RATE);
            engine_set_bpm(120.f);
            engine_set_playing(1);
            engine_render(pcm, RATE * 4);
            int cs = k == 3 ? 0 : 1, edges = 0, prev = 0, width = 0, last = -1, other = 0;
            double gaps = 0;
            float apk = 0.f;
            for (int i = 0; i < RATE * 4; i++) {
                int c = pcm[2 * i + cs], hi = c > 16000;
                other += c != 0 && c != 30400;
                if (hi && !prev) {
                    if (last >= 0)
                        gaps += i - last;
                    last = i, edges++;
                }
                if (hi && edges == 1)
                    width++;
                prev = hi;
                if (abs(pcm[2 * i + 1 - cs]) > apk)
                    apk = (float)abs(pcm[2 * i + 1 - cs]);
            }
            printf("clock out: %d PPQN at 120 BPM on the %s -> %d pulses in 4 s, mean gap %.2f ms, width %.2f ms, "
                   "other values %d, sound peak %.2f\n",
                   clk_ppqn_value(song.gp[GP_PPQN]), cs ? "right" : "left", edges, gaps / (edges - 1) * 1000.0 / RATE,
                   width * 1000.0 / RATE, other, apk / 32768.f);
        }
        engine_set_playing(0);
        engine_render(pcm, RATE / 2);
        int any = 0;
        for (int i = 0; i < RATE / 2; i++)
            any += pcm[2 * i] > 16000;
        printf("clock out stopped: pulses %d (want 0)\n", any);
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
    }
    /* the LATCH screen: 1 plucked, 3 bowed, 4 fading, the rest silent */
    {
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        song.gp[GP_PLAYMODE] = PLAY_LATCH;
        song.gp[GP_CHANGES] = 0;
        song.sp[3][SP_RELEASE] = 85;
        engine_latch(0, LATCH_PLUCK);
        engine_latch(2, LATCH_BOW);
        engine_latch(3, LATCH_BOW);
        int16_t buf[2 * 480];
        for (int i = 0; i < 250; i++) {
            engine_render(buf, 480);
            ui_tick(10);
            if (i == 150)
                engine_latch(3, LATCH_BOW); /* let 4 go */
        }
        shot(out, "latch", fb);
        engine_unlatch_all();
        song.gp[GP_PLAYMODE] = PLAY_RINGS;
    }
    /* chord memory: record four chords, then play them back in order */
    {
        song_default();
        engine_prog_mode(PROG_RECORD);
        for (int i = 0; i < 3; i++)
            engine_next_chord();
        int n = song.prog_n, rec[4][NS];
        memcpy(rec, song.prog_note, sizeof rec);
        engine_prog_mode(PROG_PLAY);
        int match = 0;
        for (int i = 0; i < 8; i++) {
            engine_next_chord();
            match += !memcmp(song.sp[0], song.sp[0], 4) && song.sp[1][SP_NOTE] == rec[i % n][1] && song.sp[3][SP_NOTE] == rec[i % n][3];
        }
        printf("chord memory: %d chords kept [%s %s %s %s], 8 changes in PLAY matched the memory %d/8\n", n, song.prog_name[0],
               song.prog_name[1], song.prog_name[2], song.prog_name[3], match);
    }
    /* key lock and mode: EQUAL in D Phrygian, locked; chords stay in mode and in tune */
    {
        song_default();
        engine_set_scale(0);
        song.gp[GP_ROOT] = 50;
        song.gp[GP_MODALITY] = MODAL_PHRYGIAN;
        song.gp[GP_KEYLOCK] = 1;
        static const int phryg[7] = {0, 1, 3, 5, 7, 8, 10};
        int out_of_mode = 0, detuned = 0;
        for (int i = 0; i < 40; i++) {
            engine_next_chord();
            for (int s = 0; s < NS; s++) {
                int pc = ((song.sp[s][SP_NOTE] % 12) + 12) % 12, in = 0;
                for (int k = 0; k < 7; k++)
                    in |= pc == phryg[k];
                out_of_mode += !in;
                detuned += song.sp[s][SP_FINE] != 0;
            }
        }
        printf("key lock, D Phrygian: notes outside the mode %d, detuned notes %d (of 200)\n", out_of_mode, detuned);
    }
    /* drone release: RELEASE 0 vs 100 (10 s), level 1 s after START stops it */
    for (int rel = 0; rel <= 100; rel += 100) {
        song_default();
        song.gp[GP_RELEASE] = rel;
        song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        engine_init(RATE);
        engine_set_playing(1);
        engine_render(pcm, RATE * 3);
        double on = rms_of(pcm, RATE * 2, RATE);
        engine_set_playing(0);
        engine_render(pcm, RATE);
        printf("release %3d: rms playing %.0f, 0.9 s after stop %.0f\n", rel, on, rms_of(pcm, RATE * 9 / 10, RATE / 10));
    }

    /* rendering ahead: at 60% of real time (heavier than any patch measured
     * on the Miyoo), made in the driver's paired requests the second buffer
     * of each pair comes late; made ahead it comes in time. And the sound is
     * the same, sample for sample, as made directly in the same pieces. */
    {
        slow_share = 0.6;
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        int late_direct = paired_requests(0, 1.0, NULL);
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        ahead_start(slow_render, RATE, 256, 1024, 0);
        int late_ahead = paired_requests(1, 1.0, NULL);
        ahead_stop();
        slow_share = 0;
        static int16_t direct[RATE * 2 * 2], via[RATE * 2 * 2];
        const int frames = RATE * 2;
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        ahead_start(slow_render, RATE, 256, 1024, 0);
        for (int i = 0; i < frames; i += 1024) {
            while (ahead_ready() < 1024) /* no deadline here: this compares the sound only */
                sleep_until(mono_s() + 0.001);
            ahead_read(via + 2 * i, 1024);
        }
        AheadStats as;
        ahead_take_stats(&as);
        ahead_stop();
        song_default();
        engine_init(RATE);
        engine_set_playing(1);
        for (int i = 0; i < frames; i += 256)
            engine_render(direct + 2 * i, 256);
        int same = as.short_frames == 0 && memcmp(direct, via, (size_t)frames * 4) == 0;
        printf("render ahead: at 60%% load, late pairs made in the request %d of 23, made ahead %d (want under 2); "
               "same sound %d\n", late_direct, late_ahead, same);

        /* the TrimUI's split: the voices and the effects on two threads, a
         * block apart, and still the same sound, sample for sample, through
         * a busy patch (every voice family, every effect up, a chord change) */
        song_default();
        for (int p = 0; p < NFX; p++)
            song.fx[p] = fparam_def(p)->max;
        song.sp[1][SP_VOICE] = 4, song.sp[2][SP_VOICE] = VOICE_PERC(PC_BOWL);
        song.gp[GP_CHANGES] = 1;
        engine_init(RATE);
        engine_set_playing(1);
        int split_ok = ahead_start_split(RATE, 256, 1024, 0, -1, -1);
        for (int i = 0; i < frames; i += 1024) {
            while (ahead_ready() < 1024)
                sleep_until(mono_s() + 0.001);
            ahead_read(via + 2 * i, 1024);
        }
        ahead_take_stats(&as);
        ahead_stop();
        int split_short = (int)as.short_frames;
        song_default();
        for (int p = 0; p < NFX; p++)
            song.fx[p] = fparam_def(p)->max;
        song.sp[1][SP_VOICE] = 4, song.sp[2][SP_VOICE] = VOICE_PERC(PC_BOWL);
        song.gp[GP_CHANGES] = 1;
        engine_init(RATE);
        engine_set_playing(1);
        for (int i = 0; i < frames; i += 256)
            engine_render(direct + 2 * i, 256);
        int split_same = split_short == 0 && memcmp(direct, via, (size_t)frames * 4) == 0;
        printf("render split: two threads %d, voices %.1f%% and effects %.1f%% of the work; same sound %d\n", split_ok,
               100.0 * as.us_voices / fmax(1, as.us_voices + as.us_effects),
               100.0 * as.us_effects / fmax(1, as.us_voices + as.us_effects), split_same);
    }

    /* USB MIDI (midi.c), bytes in as from a port: a key plays the selected
     * string's sound at its pitch; a clock sets the tempo; START and STOP start
     * and stop the drone; and the clock out ticks 24 times a beat */
    {
        song_default();
        song.gp[GP_CHANGES] = 0, song.gp[GP_DRIFT] = 0, song.lfo[0][LP_DEPTH] = 0;
        song.fx[FX_SCOUNT] = 0, song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        for (int s = 0; s < NS; s++) /* one string, its rings at rest: the key alone */
            song.on[s] = s == 0, song.sp[s][SP_PON] = song.sp[s][SP_BON] = 0, song.sp[s][SP_BOW] = 0;
        engine_init(RATE);
        engine_set_playing(1);
        midi_reset();
        engine_render(pcm, RATE / 2);
        uint64_t t = 1000000;
        const uint8_t on[] = {0x90, 57, 100}; /* A3, 220 Hz */
        midi_feed(on, 3, t);
        engine_render(pcm, RATE * 2);
        double key_hz = hz_yin(pcm, RATE, RATE / 4);
        const uint8_t off[] = {0x80, 57, 0};
        midi_feed(off, 3, t + 2000000);
        printf("midi key: A3 -> %.1f Hz (want 220: %d), string tuned %.2f\n", key_hz, fabs(key_hz - 220.0) < 2.0,
               engine_string_pitch(0));

        /* the keys: each plays the voice its channel plays (the selected one,
         * or a voice's own channel), at most four keys a voice, on key
         * voices of their own; they sound with the drone stopped and never
         * start it; velocity, aftertouch, the mod wheel and the bend range
         * as the MIDI page sets them */
        {
            uint64_t tk = 5000000;
            uint8_t m[3] = {0x90, 0, 100};
#define SEND(a, b, c) (m[0] = (uint8_t)(a), m[1] = (uint8_t)(b), m[2] = (uint8_t)(c), midi_feed(m, 3, tk += 1000))
#define SEND2(a, b) (m[0] = (uint8_t)(a), m[1] = (uint8_t)(b), midi_feed(m, 2, tk += 1000))
#define KEY_ON(ch, k, v) (SEND(0x90 | (ch), k, v), midi_voice_of(ch, k))
#define KEY_OFF(ch, k) SEND(0x80 | (ch), k, 0)
#define FRESH()                                                                                                       \
    (song_default(), song.gp[GP_CHANGES] = 0, song.fx[FX_SCOUNT] = 0, song.gp[GP_DRIFT] = 0,                          \
     song.lfo[0][LP_DEPTH] = 0, engine_init(RATE), engine_set_playing(0), midi_reset())
            MidiCfg keep = midi_cfg;
            FRESH();
            int sel = ui_selected_string(), v[5];
            static const int chord[5] = {57, 61, 64, 69, 72};
            for (int k = 0; k < 5; k++)
                v[k] = KEY_ON(0, chord[k], 100);
            int apart = 1;
            for (int k = 0; k < 4; k++)
                for (int j = 0; j < k; j++)
                    apart &= v[k] != v[j] && v[k] >= NS && v[k] < NV;
            int steal_ok = v[4] == v[0] && midi_voice_of(0, chord[0]) < 0; /* a fifth on one voice: the oldest gives way */
            for (int k = 1; k < 5; k++)
                KEY_OFF(0, chord[k]);
            /* the strings all muted, the drone stopped: a key sounds, at its pitch, and the drone stays stopped */
            FRESH();
            for (int s = 0; s < NS; s++)
                song.on[s] = 0;
            engine_render(pcm, RATE / 2);
            double before = rms_of(pcm, 0, RATE / 2);
            int kv = KEY_ON(0, 57, 100);
            engine_render(pcm, RATE);
            double key_rms = rms_of(pcm, RATE / 4, RATE / 2), key_hz = hz_yin(pcm, RATE / 2, RATE / 4);
            int stopped = !live.playing;
            KEY_OFF(0, 57);
            for (int i = 0; i < 40 && engine_key_sounding(kv); i++)
                engine_render(pcm, RATE / 4);
            int stops = !engine_key_sounding(kv);
            int sound_ok = before < 1.0 && key_rms > 100.0 && fabs(key_hz - 220.0) < 3.0 && stops && stopped;
            /* the selected voice's LEVEL at 0: its keys are silent; voice 2 on channel 2, its LEVEL kept: channel 2 sounds */
            double lv[2];
            int other = (sel + 1) % NS;
            for (int pass = 0; pass < 2; pass++) {
                FRESH();
                for (int s = 0; s < NS; s++)
                    song.on[s] = 0;
                song.sp[sel][SP_LEVEL] = 0;
                midi_cfg.vch[other] = 2;
                KEY_ON(pass, 57, 100), KEY_ON(pass, 64, 100);
                engine_render(pcm, RATE / 2);
                lv[pass] = rms_of(pcm, RATE / 4, RATE / 4);
                KEY_OFF(pass, 57), KEY_OFF(pass, 64);
                midi_cfg.vch[other] = 0;
            }
            int chan_ok = lv[0] < 5.0 && lv[1] > 100.0;
            /* velocity: a soft key quieter than a hard one, unless VELOCITY is 0 */
            double vr[2][2];
            for (int sens = 0; sens < 2; sens++)
                for (int hard = 0; hard < 2; hard++) {
                    FRESH();
                    for (int s = 0; s < NS; s++)
                        song.on[s] = 0;
                    midi_cfg.vel = sens ? 100 : 0;
                    KEY_ON(0, 57, hard ? 127 : 20);
                    engine_render(pcm, RATE / 4);
                    vr[sens][hard] = rms_of(pcm, 0, RATE / 4);
                    KEY_OFF(0, 57);
                }
            midi_cfg.vel = 100;
            int vel_ok = vr[1][1] > 1.3 * vr[1][0] && fabs(vr[0][1] / vr[0][0] - 1.0) < 0.05;
            /* aftertouch to PITCH: two semitones up at full pressure; bend at 12: an octave */
            FRESH();
            for (int s = 0; s < NS; s++)
                song.on[s] = 0;
            midi_cfg.at_to = MT_PITCH, midi_cfg.bend = 12;
            KEY_ON(0, 57, 100);
            SEND2(0xd0, 127); /* channel pressure: full */
            engine_render(pcm, RATE);
            double at_hz = hz_yin(pcm, RATE / 2, RATE / 4);
            SEND2(0xd0, 0);
            SEND(0xe0, 127, 127); /* bend all the way up */
            engine_render(pcm, RATE);
            double bend_hz = hz_yin(pcm, RATE / 2, RATE / 4);
            KEY_OFF(0, 57);
            midi_cfg.at_to = MT_BOW, midi_cfg.bend = 2;
            /* the mod wheel to TONE, and one key's own aftertouch to TONE: that key's BRIGHT only */
            FRESH();
            midi_cfg.mod_to = MT_TONE, midi_cfg.at_to = MT_TONE;
            int ka = KEY_ON(0, 60, 100), kb = KEY_ON(0, 67, 100);
            int bright0 = engine_param_played(ka, SP_BRIGHT);
            SEND(0xa0, 60, 127); /* key 60 pressed hard */
            int poly_a = engine_param_played(ka, SP_BRIGHT), poly_b = engine_param_played(kb, SP_BRIGHT);
            SEND(0xb0, 1, 127); /* the mod wheel up: both */
            int mod_b = engine_param_played(kb, SP_BRIGHT);
            KEY_OFF(0, 60), KEY_OFF(0, 67);
            midi_cfg = keep;
            int expr_ok = fabs(at_hz / 220.0 - 1.1225) < 0.01 && fabs(bend_hz / 440.0 - 1.0) < 0.01 && poly_a == 100 &&
                          poly_b == bright0 && mod_b == 100;
#undef KEY_ON
#undef KEY_OFF
#undef SEND
#undef SEND2
#undef FRESH
            printf("midi keys: selected %d, four on key voices %d %d %d %d (apart 1: %d), a fifth on the voice takes the "
                   "oldest's %d (1: %d); drone stopped and strings muted, a key rms %.0f at %.1f Hz, the drone still "
                   "stopped and let go it stops (1: %d); selected LEVEL 0 rms %.0f, voice %d on channel 2 %.0f (1: %d); "
                   "velocity soft/hard %.0f/%.0f, at VELOCITY 0 %.0f/%.0f (1: %d); aftertouch PITCH %.1f Hz, bend 12 "
                   "%.1f Hz, key aftertouch TONE %d/%d, mod wheel %d (1: %d): %d\n",
                   sel, v[0], v[1], v[2], v[3], apart, v[4], steal_ok, key_rms, key_hz, sound_ok, lv[0], other + 1,
                   lv[1], chan_ok, vr[1][0], vr[1][1], vr[0][0], vr[0][1], vel_ok, at_hz, bend_hz, poly_a, poly_b,
                   mod_b, expr_ok, apart && steal_ok && sound_ok && chan_ok && vel_ok && expr_ok);
            song_default();
            midi_reset();
        }

        /* how the keys play (the MIDI page): SHIFT, SNAP to the scale, PLAY BOW
         * swelling in where a pluck starts at once, HOLD LATCH droning on
         * after the key is let go until it comes again, POLY 1 */
        {
            uint64_t tk = 9000000;
            uint8_t m[3];
#define SEND(a, b, c) (m[0] = (uint8_t)(a), m[1] = (uint8_t)(b), m[2] = (uint8_t)(c), midi_feed(m, 3, tk += 1000))
#define FRESH()                                                                                                       \
    (song_default(), song.gp[GP_CHANGES] = 0, song.fx[FX_SCOUNT] = 0, song.gp[GP_DRIFT] = 0,                          \
     song.lfo[0][LP_DEPTH] = 0, engine_init(RATE), engine_set_playing(0), midi_reset())
#define MUTE_ALL() for (int s_ = 0; s_ < NS; s_++) song.on[s_] = 0
            MidiCfg keep = midi_cfg;
            FRESH();
            MUTE_ALL();
            midi_cfg.shift = 12;
            SEND(0x90, 57, 100);
            engine_render(pcm, RATE);
            double sh_hz = hz_yin(pcm, RATE / 2, RATE / 4);
            SEND(0x80, 57, 0);
            midi_cfg.shift = 0;
            /* SNAP: a key between two of the scale's notes moves onto one; one on it stays */
            float on = (float)song.gp[GP_ROOT], off = on + 1.f, sn_on = engine_scale_snap(on), sn_off = engine_scale_snap(off);
            int snap_ok = fabsf(sn_on - on) < 1e-3f && fabsf(sn_off - off) > 0.2f && fabsf(sn_off - off) <= 1.f;
            /* PLAY BOW against PLUCK: the first 30 ms */
            double first[2];
            for (int b = 0; b < 2; b++) {
                FRESH();
                MUTE_ALL();
                midi_cfg.play = b ? KP_BOW : KP_PLUCK;
                SEND(0x90, 57, 110);
                engine_render(pcm, RATE * 3 / 100);
                first[b] = rms_of(pcm, 0, RATE * 3 / 100);
                SEND(0x80, 57, 0);
            }
            midi_cfg.play = KP_BOTH;
            int bow_ok = first[1] < 0.3 * first[0];
            /* HOLD LATCH, in RINGS: drones after the key is let go; the key again lets it go */
            FRESH();
            MUTE_ALL();
            midi_cfg.hold = KH_LATCH, midi_cfg.play = KP_BOW;
            int lv = (SEND(0x90, 57, 100), midi_voice_of(0, 57));
            SEND(0x80, 57, 0);
            engine_render(pcm, RATE * 3);
            double latched = rms_of(pcm, RATE * 2, RATE / 2);
            SEND(0x90, 57, 100); /* again: let go */
            SEND(0x80, 57, 0);
            for (int i = 0; i < 80 && engine_key_sounding(lv); i++)
                engine_render(pcm, RATE / 4);
            int latch_ok = latched > 100.0 && !engine_key_sounding(lv);
            midi_cfg.hold = KH_SONG, midi_cfg.play = KP_BOTH;
            /* POLY 1: a second key takes the first one's place */
            FRESH();
            midi_cfg.poly = 1;
            int p1 = (SEND(0x90, 60, 100), midi_voice_of(0, 60)), p2 = (SEND(0x90, 64, 100), midi_voice_of(0, 64));
            int poly_ok = p1 == p2 && midi_voice_of(0, 60) < 0;
            SEND(0x80, 64, 0);
            midi_cfg = keep;
#undef SEND
#undef FRESH
#undef MUTE_ALL
            printf("midi key play: SHIFT +12 %.1f Hz (want 440), SNAP %.2f -> %.2f, %.2f stays (1: %d), PLAY BOW "
                   "first 30 ms %.0f against a pluck's %.0f (1: %d), HOLD LATCH drones %.0f after the key, let go "
                   "by it again (1: %d), POLY 1 one voice (1: %d): %d\n",
                   sh_hz, off, sn_off, on, snap_ok, first[1], first[0], bow_ok, latched, latch_ok, poly_ok,
                   fabs(sh_hz - 440.0) < 4.0 && snap_ok && bow_ok && latch_ok && poly_ok);
            song_default();
            midi_reset();
        }

        /* the MIDI page: START + B on a SOUND control, then a CC learns it;
         * the CC then moves it on the voice of the channel it comes on;
         * kept in midi.cfg and read back */
        {
            song_default();
            MidiCfg keep = midi_cfg;
            for (int i = 0; i < PAGE_COUNT && ui_page() != PAGE_SOUND; i++)
                press(BTN_R1);
            for (int i = 0; i < 12; i++)
                press(BTN_UP);
            press(BTN_DOWN); /* the NOTE row ... */
            press(BTN_DOWN), press(BTN_DOWN), press(BTN_DOWN), press(BTN_DOWN); /* ... down to BRIGHT */
            ui_button(BTN_START, 1), press(BTN_B), ui_button(BTN_START, 0);
            int learning = ui_midi_learning();
            int took = ui_midi_cc(0, 40, 0); /* CC 40 comes: learned */
            midi_cfg.vch[2] = 4;
            int got = ui_midi_cc(3, 40, 127); /* channel 4: voice 3's BRIGHT */
            int b3 = song.sp[2][SP_BRIGHT], b1 = song.sp[0][SP_BRIGHT];
            ui_tick(1200); /* written */
            midi_cfg = keep;
            midi_cfg.bend = 7;
            ui_midi_load();
            int kept = midi_cfg.vch[2] == 4 && midi_cfg.bend == 2;
            song.sp[2][SP_BRIGHT] = 50;
            midi_cfg.vch[2] = 4;
            ui_midi_cc(3, 40, 0);
            int kept_map = song.sp[2][SP_BRIGHT] == 0;
            char cp[512];
            snprintf(cp, sizeof cp, "%s/midi.cfg", out);
            remove(cp);
            midi_cfg = keep;
            while (ui_page() != PAGE_PLAY)
                press(BTN_R1);
            int ok = learning && took && got && b3 == 100 && b1 != 100 && kept && kept_map;
            printf("midi learn: START+B learns %d, CC 40 taken %d, on channel 4 sets voice 3's BRIGHT %d (others %d); "
                   "kept in midi.cfg %d, the map too %d: %d\n",
                   learning, took, b3, b1, kept, kept_map, ok);
            song_default();
        }

        { /* MIDI CC: a setting by its CC, on the selected string; the fixed CCs stay put */
            song_default();
            uint8_t m[3] = {0xb5, 2, 127}; /* channel 6: any channel is heard */
            int mch, mcc, mv, n = 0;
            midi_feed(m, 3, 1000);           /* STRING: 5 */
            m[1] = 16, m[2] = 0;             /* its LEVEL: 0 */
            midi_feed(m, 3, 2000);
            m[1] = 90, m[2] = 127;           /* BPM: the top */
            midi_feed(m, 3, 3000);
            m[1] = 29, m[2] = 0;             /* string 5 muted */
            midi_feed(m, 3, 4000);
            m[1] = 74, m[2] = 100;           /* TONE: a fixed CC, not queued */
            midi_feed(m, 3, 5000);
            while (midi_take_cc(&mch, &mcc, &mv))
                n += ui_midi_cc(mch, mcc, mv);
            int ok = n == 4 && ui_selected_string() == NS - 1 && song.sp[NS - 1][SP_LEVEL] == 0 &&
                     song.sp[0][SP_LEVEL] != 0 && song.gp[GP_BPM] == 200 && !song.on[NS - 1] && song.on[0];
            printf("midi cc: string 5 selected, its level 0, BPM 200, muted; 74 not queued: %d\n", ok);
            song_default();
            midi_reset();
        }

        song_default();
        engine_init(RATE);
        engine_set_playing(0);
        midi_reset();
        const uint8_t start = 0xfa, stop = 0xfc, tick = 0xf8;
        midi_feed(&start, 1, t);
        int started = live.playing;
        for (int i = 0; i < 24 * 8; i++) { /* 8 beats at 120 BPM: a tick each 20.833 ms */
            t += 20833;
            midi_feed(&tick, 1, t);
            engine_render(pcm, 1000); /* about the time between ticks */
        }
        float bpm_in = engine_bpm();
        midi_feed(&stop, 1, t);
        printf("midi clock in: START plays %d, 120 BPM -> %.1f (%d), STOP stops %d\n", started, bpm_in,
               fabsf(bpm_in - 120.f) < 0.5f, !live.playing);

        song_default();
        song.gp[GP_BPM] = 120;
        engine_init(RATE);
        engine_set_playing(1);
        midi_reset();
        midi_set_latency(0.f);
        uint8_t out[64];
        int ticks_out = 0, starts = 0, stops = 0;
        t = 5000000;
        for (int i = 0; i < 400; i++) { /* 4 s in 10 ms steps */
            engine_render(pcm, RATE / 100);
            t += 10000;
            int n = midi_clock_step(t, out, sizeof out);
            for (int k = 0; k < n; k++)
                ticks_out += out[k] == 0xf8, starts += out[k] == 0xfa;
        }
        engine_set_playing(0);
        int n = midi_clock_step(t + 10000, out, sizeof out);
        for (int k = 0; k < n; k++)
            stops += out[k] == 0xfc;
        printf("midi clock out: START %d, %d ticks in 4 s at 120 BPM (want 192: %d), STOP %d\n", starts, ticks_out,
               abs(ticks_out - 192) <= 1, stops);
        midi_reset();

    /* ---- the SONG page's MIDI setting: AUTO follows a clock coming in and
     * sends one, SEND only sends (and ignores one coming in), FOLLOW only
     * follows, OFF neither; the setting cycles on the page with X */
    {
        static int16_t pcm[2048 * 2 * 24];
        const uint8_t start = 0xfa, stop = 0xfc, tick = 0xf8;
        int got[NMIDIMODE][2], ok = 1;
        static const int want[NMIDIMODE][2] = {{1, 1}, {0, 1}, {1, 0}, {0, 0}}; /* follows, sends */
        for (int m = 0; m < NMIDIMODE; m++) {
            song_default();
            song.gp[GP_MIDI] = m;
            engine_init(RATE);
            engine_set_playing(0);
            midi_reset();
            midi_set_latency(0.f);
            uint64_t t = 1000000;
            midi_feed(&start, 1, t);
            for (int i = 0; i < 48; i++) {
                t += 20833;
                midi_feed(&tick, 1, t);
                engine_render(pcm, 1000);
            }
            got[m][0] = midi_following() && live.playing;
            midi_feed(&stop, 1, t);
            engine_set_playing(0);
            midi_reset();
            engine_set_playing(1);
            uint8_t out[64];
            int ticks = 0;
            for (int i = 0; i < 100; i++) {
                engine_render(pcm, RATE / 100);
                t += 10000;
                int n = midi_clock_step(t, out, sizeof out);
                for (int k = 0; k < n; k++)
                    ticks += out[k] == 0xf8;
            }
            engine_set_playing(0);
            midi_clock_step(t + 10000, out, sizeof out);
            got[m][1] = ticks > 0;
            ok &= got[m][0] == want[m][0] && got[m][1] == want[m][1];
        }
        midi_reset();
        /* saved with the song, and a v0.13 save (JDR17) loads with AUTO and FOLLOW */
        song_default();
        song.gp[GP_MIDI] = MIDI_FOLLOW, song.gp[GP_MIDISTART] = 1;
        char sp[600];
        snprintf(sp, sizeof sp, "%s/midi_mode.jds", test_dir);
        song_save(sp);
        song_default();
        int page = song_load(sp) && song.gp[GP_MIDI] == MIDI_FOLLOW && song.gp[GP_MIDISTART] == 1;
        {
            FILE *f = fopen(sp, "rb");
            static unsigned char buf[sizeof(Song)];
            size_t got = f ? fread(buf, 1, sizeof buf, f) : 0;
            if (f)
                fclose(f);
            size_t cut = offsetof(Song, gp) + 32 * sizeof(int); /* JDR17 had 32 global params */
            uint32_t magic = 0x4a445211u;
            memcpy(buf, &magic, 4);
            memmove(buf + cut, buf + cut + 2 * sizeof(int), got - cut - 2 * sizeof(int));
            f = fopen(sp, "wb");
            if (f) {
                fwrite(buf, 1, got - 2 * sizeof(int), f);
                fclose(f);
            }
            song.gp[GP_MIDI] = MIDI_OFF;
            page &= song_load(sp) && song.gp[GP_MIDI] == MIDI_AUTO && song.gp[GP_MIDISTART] == 0 &&
                    song.gp[GP_PULSE] == 5;
        }
        song_default();
        printf("midi modes (follows, sends): AUTO %d %d, SEND %d %d, FOLLOW %d %d, OFF %d %d; saved, and an older save loads %d "
               "(want 1: %d)\n",
               got[0][0], got[0][1], got[1][0], got[1][1], got[2][0], got[2][1], got[3][0], got[3][1], page,
               ok && page);
    }
    /* ---- the Brick clock link and MIDI clock-only (midi.c, link.c) */
    {
        static int16_t pcm[2048 * 2 * 24];
        const uint8_t start = 0xfa, stop = 0xfc, tick = 0xf8;
        /* clock only: START and STOP pass by, the tempo still follows */
        aclock_init(RATE, 0); /* nothing from the sync tests: the engine's position less the latency */
        song_default();
        song.gp[GP_MIDISTART] = 1;
        engine_init(RATE);
        engine_set_playing(0);
        midi_reset();
        midi_set_latency(0.f);
        uint64_t t = 1000000;
        midi_feed(&start, 1, t);
        int start_ignored = !live.playing;
        for (int i = 0; i < 48; i++) {
            t += 20833;
            midi_feed(&tick, 1, t);
            engine_render(pcm, 1000);
        }
        int tempo = midi_following() && fabsf(engine_bpm() - 120.f) < 0.2f;
        engine_set_playing(1); /* started by hand */
        midi_feed(&stop, 1, t);
        int stop_ignored = live.playing;
        engine_set_playing(0);

        /* the link: a leader at 137.37 BPM sends where its sound is every 20 ms;
         * this one, stopped, joins it mid-song and holds it */
        static const double tempos[] = {137.37, 200.0, 224.5, 250.0};
        double worst_ms_at[4], worst_ms = 0.0, bpm = 0.0, lat = 512.0 / RATE, lead0 = 37.25;
        int joined = 1, exact_tempo = 1;
        uint8_t msg[16];
        for (int k = 0; k < 4; k++) {
            bpm = tempos[k];
            song_default();
            song.gp[GP_MIDISTART] = 0;
            engine_init(RATE);
            engine_set_playing(0);
            midi_reset();
            midi_set_latency((float)lat);
            t = 5000000;
            double worst = 0.0;
            for (int i = 0; i < 400; i++) { /* 8 s */
                double lead = lead0 + (double)(t - 5000000) * 1e-6 * bpm / 15.0;
                /* after a few seconds, how far this one's sound is from the leader's, now */
                if (i > 150) {
                    double heard = engine_position() - lat * engine_bpm() / 15.0;
                    double e = fabs(lead - heard);
                    worst = e > worst ? e : worst;
                }
                int n = midi_link_pos(msg, sizeof msg, lead, bpm, 1);
                /* it arrives 0.15 to 0.45 ms after it was sent (USB through the hub and
                 * the thread waking), at random, while this one plays on */
                int late = (int)(0.00015 * RATE) + rand() % (int)(0.0003 * RATE + 1);
                engine_render(pcm, late);
                midi_feed(msg, n, t + (uint64_t)late * 1000000 / RATE);
                if (i == 0)
                    joined &= live.playing;
                engine_render(pcm, RATE / 50 - late);
                t += 20000;
            }
            worst_ms_at[k] = worst * 15000.0 / bpm;
            worst_ms = fmax(worst_ms, worst_ms_at[k]);
            exact_tempo &= fabs(engine_bpm() - bpm) < 0.001;
        }
        /* the leader stops: so does this one */
        int n = midi_link_pos(msg, sizeof msg, 0.0, bpm, 0);
        midi_feed(msg, n, t);
        int stops = !live.playing;

        /* clock only over the link: started by hand somewhere else, it lines up the bar */
        song.gp[GP_MIDISTART] = 1;
        midi_reset();
        engine_init(RATE);
        engine_set_playing(0);
        engine_set_playing(1);
        engine_set_position(5.3);
        double bar_err = 99.0;
        for (int i = 0; i < 150; i++) {
            double lead = lead0 + (double)(t - 5000000) * 1e-6 * bpm / 15.0;
            n = midi_link_pos(msg, sizeof msg, lead, bpm, 1);
            midi_feed(msg, n, t);
            engine_render(pcm, RATE / 50);
            t += 20000;
            if (i == 149) {
                double heard = engine_position() - lat * engine_bpm() / 15.0;
                double d = lead + (20000 * 1e-6 + 0.0003) * bpm / 15.0 - heard;
                bar_err = fabs(d - 16.0 * floor((d + 8.0) / 16.0));
            }
        }
        n = midi_link_pos(msg, sizeof msg, 0.0, bpm, 0);
        midi_feed(msg, n, t);
        int no_stop = live.playing; /* the clock only: the leader stopping doesn't stop it */
        engine_set_playing(0);
        song.gp[GP_MIDISTART] = 0;

        /* a Brick following another clock passes it on to the ones linked to it */
        midi_reset();
        midi_set_latency(0.f);
        engine_init(RATE);
        t = 9000000;
        midi_feed(&start, 1, t);
        for (int i = 0; i < 48; i++) {
            t += 20833;
            midi_feed(&tick, 1, t);
            engine_render(pcm, 1000);
        }
        n = midi_link_step(t + 1, msg, sizeof msg);
        int relays = midi_following() && n == 14 && msg[0] == 0xf0 && msg[1] == 0x7d && msg[13] == 0xf7;
        midi_feed(&stop, 1, t);
        midi_reset();

        /* the follower's USB descriptors, and the leader finding a linked Brick
         * in a stand-in sysfs: not the test memory stick with the same ids */
        uint8_t d[256];
        int dn = link_descriptors(d, sizeof d), sn = link_strings(d + 128, 128);
        /* the head (12 bytes) and the full- and high-speed counts (8), then the
         * interface (9) and its bulk OUT endpoint (7), for each speed */
        int desc = dn == 52 && d[0] == 3 && d[20 + 5] == 0xff && d[20 + 6] == LINK_SUBCLASS &&
                   d[20 + 7] == LINK_PROTOCOL && d[29 + 2] == 0x01 && d[29 + 3] == 2 && d[45 + 4] == 0 &&
                   d[45 + 5] == 2 && sn > 16;
        char fs[600], fp[800];
        snprintf(fs, sizeof fs, "%s.fakeusb", test_dir);
        mkdir(fs, 0755);
        const char *const tree[][2] = {
            {"1-1.3/idVendor", "1d6b"},           {"1-1.3/idProduct", "0104"},
            {"1-1.3/busnum", "1"},                {"1-1.3/devnum", "5"},
            {"1-1.3/1-1.3:1.0/bInterfaceClass", "ff"}, {"1-1.3/1-1.3:1.0/bInterfaceSubClass", "42"},
            {"1-1.3/1-1.3:1.0/bInterfaceProtocol", "4c"}, {"1-1.3/1-1.3:1.0/ep_02/direction", "out"},
            {"1-1.3/1-1.3:1.0/ep_02/type", "Bulk"}, {"1-1.3/1-1.3:1.0/ep_02/bEndpointAddress", "02"},
            {"1-1.4/idVendor", "1d6b"},           {"1-1.4/idProduct", "0104"},
            {"1-1.4/busnum", "1"},                {"1-1.4/devnum", "6"},
            {"1-1.4/1-1.4:1.0/bInterfaceClass", "08"},
        };
        for (size_t k = 0; k < sizeof tree / sizeof tree[0]; k++) {
            snprintf(fp, sizeof fp, "%s/%s", fs, tree[k][0]);
            for (char *c = fp + strlen(fs) + 1; *c; c++)
                if (*c == '/') {
                    *c = 0;
                    mkdir(fp, 0755);
                    *c = '/';
                }
            FILE *f = fopen(fp, "w");
            if (f) {
                fprintf(f, "%s\n", tree[k][1]);
                fclose(f);
            }
        }
        LinkDev ld[4];
        int found = link_find(fs, ld, 4);
        int finds = found == 1 && ld[0].bus == 1 && ld[0].dev == 5 && ld[0].ep == 0x02;
        /* plain MIDI clock from outside gear at 250 BPM (a tick every 10 ms): how close the ticks alone hold it */
        song_default();
        engine_init(RATE);
        engine_set_playing(0);
        midi_reset();
        midi_set_latency(0.f);
        t = 20000000;
        midi_feed(&start, 1, t);
        engine_render(pcm, 64);
        /* each tick arrives 0 to 2 ms late at random (USB, the thread waking) while this one plays on;
         * how much its sound wobbles around its average offset (that average is the cable's own delay) */
        static double tk_err[24 * 8];
        int ne = 0;
        for (int i = 1; i <= 24 * 32; i++) {
            int late = rand() % (int)(0.002 * RATE + 1);
            engine_render(pcm, late);
            midi_feed(&tick, 1, t + 10000 + (uint64_t)late * 1000000 / RATE);
            engine_render(pcm, RATE / 100 - late);
            t += 10000;
            if (i > 24 * 24 && ne < 24 * 8)
                tk_err[ne++] = engine_position() - i / 6.0;
        }
        midi_feed(&stop, 1, t);
        midi_reset();
        double mean = 0.0, wob = 0.0;
        for (int k = 0; k < ne; k++)
            mean += tk_err[k] / ne;
        for (int k = 0; k < ne; k++)
            wob = fmax(wob, fabs(tk_err[k] - mean));
        double tick_ms = wob * 15000.0 / 250.0;
        printf("midi clock in at 250 BPM: the tempo %.1f, the sound within %.3f ms of steady, ticks up to 2 ms late "
               "(want under 0.5: %d)\n",
               engine_bpm(), tick_ms, tick_ms < 0.5);
        printf("clock only: START ignored %d, STOP ignored %d, the tempo followed %d (want 1: %d)\n", start_ignored,
               stop_ignored, tempo, start_ignored && stop_ignored && tempo);
        printf("link sync: joins mid-song %d, the exact tempo %d, the sound within %.3f / %.3f / %.3f / %.3f ms of the "
               "leader's at 137.37 / 200 / 224.5 / 250 BPM (want under 0.25: %d), stops with it %d; clock only: the bar within %.3f steps %d, keeps playing %d; "
               "a follower passes the clock on %d (want 1: %d)\n",
               joined, exact_tempo, worst_ms_at[0], worst_ms_at[1], worst_ms_at[2], worst_ms_at[3], worst_ms < 0.25,
               stops, bar_err, bar_err < 0.01, no_stop, relays,
               joined && exact_tempo && worst_ms < 0.25 && stops && bar_err < 0.01 && no_stop && relays);
        printf("link usb: descriptors %d (%d bytes), strings %d, a linked Brick found %d (bus %d device %d endpoint "
               "0x%02x), the memory stick passed over %d (want 1: %d)\n",
               desc, dn, sn > 16, finds, found ? ld[0].bus : 0, found ? ld[0].dev : 0, found ? ld[0].ep : 0,
               found == 1, desc && finds);
    }

        { /* JUMP: a click in a clean tone is found; a tone, a square wave, a crushed tone and the drone are not */
            static int16_t b[48000 * 2];
            unsigned seg[5];
            for (int k = 0; k < 4; k++) {
                for (int i = 0; i < 48000; i++) {
                    double ph = 2 * M_PI * 220.0 * i / 48000.0, x = k == 2 ? (sin(ph) > 0 ? 0.5 : -0.5) : 0.4 * sin(ph);
                    if (k == 3)
                        x = floor(x * 8.0) / 8.0;
                    if (k == 1 && i == 30000)
                        x += 0.5;
                    b[i * 2] = b[i * 2 + 1] = (int16_t)(x * 32767);
                }
                if (k >= 2)
                    perf_audio_output(b, 48000); /* a second of it first: the switch to it is a jump of its own */
                unsigned before = perf_jumps();
                perf_audio_output(b, 48000);
                seg[k] = perf_jumps() - before;
            }
            song_default();
            engine_init(RATE);
            engine_set_playing(1);
            for (int i = 0; i < 4; i++) /* settle */
                engine_render(b, 512), perf_audio_output(b, 512);
            unsigned before = perf_jumps();
            for (int i = 0; i < 20 * RATE / 512; i++) {
                engine_render(b, 512);
                perf_audio_output(b, 512);
            }
            seg[4] = perf_jumps() - before;
            engine_set_playing(0);
            printf("perflog jumps: tone %u, tone with a click %u, square %u, crushed %u, 20 s of the drone %u "
                   "(want 0 1 0 0 0: %d)\n", seg[0], seg[1], seg[2], seg[3], seg[4],
                   seg[0] == 0 && seg[1] == 1 && seg[2] == 0 && seg[3] == 0 && seg[4] == 0);
            song_default();
            engine_init(RATE);
        }

        /* the Brick Pro's left stick: bowing by hand, lighter or harder, and
         * a bend of two semitones */
        double lvl[2];
        for (int k = 0; k < 2; k++) {
            song_default();
            song.gp[GP_CHANGES] = 0, song.gp[GP_DRIFT] = 0, song.lfo[0][LP_DEPTH] = 0;
            song.fx[FX_SCOUNT] = 0, song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
            song.gp[GP_SUB] = 0;
            for (int s = 0; s < NS; s++)
                song.on[s] = s == 1, song.sp[s][SP_PON] = song.sp[s][SP_BON] = 0, song.sp[s][SP_BOW] = 0;
            engine_init(RATE);
            engine_set_playing(1);
            engine_bow_pressure(1, k ? 1.0f : 0.25f);
            engine_bow(1, 1);
            engine_render(pcm, RATE * 3);
            lvl[k] = live.meter[1];
            engine_bow(1, 0);
            engine_bow_pressure(1, 1.f);
        }
        song_default();
        song.gp[GP_CHANGES] = 0, song.gp[GP_DRIFT] = 0, song.lfo[0][LP_DEPTH] = 0;
        song.fx[FX_SCOUNT] = 0, song.fx[FX_DMIX] = song.fx[FX_VSEND] = 0;
        for (int s = 0; s < NS; s++)
            song.on[s] = s == 0, song.sp[s][SP_PON] = song.sp[s][SP_BON] = 0;
        engine_init(RATE);
        engine_set_playing(1);
        engine_render(pcm, RATE * 2);
        double f_flat = hz_yin(pcm, RATE, RATE / 4);
        engine_express_bend(0, 2.f);
        engine_render(pcm, RATE * 2);
        double f_bent = hz_yin(pcm, RATE, RATE / 4);
        engine_express_bend(0, 0.f);
        printf("stick: bow light %.3f, hard %.3f (lighter is softer: %d); bend +2 x%.3f (want 1.122: %d)\n", lvl[0],
               lvl[1], lvl[0] < lvl[1] * 0.8, f_bent / f_flat, fabs(f_bent / f_flat - 1.1225) < 0.006);

        /* the sticks' reach: a stick that goes only 70% of the way still bows
         * at full once it has been there; a round gate's diagonal reaches
         * both ends; the bow covers the engine's whole range */
        float sv[STK_AXES];
        sticks_reset(0.6f);
        sticks_axis(STK_LY, -22937); /* 70% up */
        sticks_read(sv);
        float up70 = -sv[STK_LY];
        sticks_axis(STK_LY, 0);
        sticks_axis(STK_RX, 23170), sticks_axis(STK_RY, -23170); /* up and right on a round gate: 0.707 each */
        sticks_axis(STK_RX, 32767), sticks_axis(STK_RX, 23170); /* (the stick has been all the way across) */
        sticks_axis(STK_RY, -32767), sticks_axis(STK_RY, -23170);
        sticks_read(sv);
        float diag_x = sv[STK_RX], diag_y = -sv[STK_RY];
        sticks_axis(STK_RX, 300), sticks_axis(STK_RY, -300); /* resting near the centre */
        sticks_read(sv);
        int rest = sv[STK_RX] == 0.f && sv[STK_RY] == 0.f;
        float bp0 = stick_bow_pressure(0.f), bp_light = stick_bow_pressure(0.05f), bp_full = stick_bow_pressure(1.f);
        printf("stick reach: 70%% stick up -> %.2f (want 1: %d); diagonal -> %.2f %.2f (want both 1: %d); rest 0: %d; "
               "bow %.2f .. %.2f .. %.2f (want 0, light, 1.5: %d)\n",
               up70, up70 > 0.99f, diag_x, diag_y, diag_x > 0.99f && diag_y > 0.99f, rest, bp0, bp_light, bp_full,
               bp0 == 0.f && bp_light > 0.f && bp_light < 0.2f && fabsf(bp_full - 1.5f) < 1e-4f);

        /* the right stick: HARM from OFF up to the 5th and back, with no
         * flicker between two steps; the mapped control's whole range */
        int held = -1000, h_half, h_full, flips = 0, last;
        h_half = stick_control(&held, 0, 0, 4, 0.5f);
        last = h_half;
        for (int i = 0; i < 200; i++) { /* a hand trembling at a step's edge */
            int v = stick_control(&held, 0, 0, 4, 0.625f + 0.05f * sinf(i * 0.7f));
            flips += v != last, last = v;
        }
        h_full = stick_control(&held, 0, 0, 4, 1.f);
        int vheld = -1000;
        int lo = stick_control(&vheld, 40, 0, 100, -1.f), mid = stick_control(&vheld, 40, 0, 100, 0.5f),
            hi = stick_control(&vheld, 40, 0, 100, 1.f);
        printf("right stick: HARM OFF -> half %d, full %d (want 2, 4: %d), flicker %d (want under 2: %d); "
               "BRIGHT 40 -> %d %d %d (want 0 70 100: %d)\n",
               h_half, h_full, h_half == 2 && h_full == 4, flips, flips < 2, lo, mid, hi, lo == 0 && mid == 70 && hi == 100);

        /* a control played from the stick sounds as the song's would, and
         * leaves the song as it was */
        double e[2][2];
        for (int k = 0; k < 2; k++) {
            song_default();
            song.gp[GP_CHANGES] = 0, song.gp[GP_DRIFT] = 0, song.lfo[0][LP_DEPTH] = 0;
            for (int s = 0; s < NS; s++)
                song.on[s] = s == 0, song.sp[s][SP_PON] = 0;
            if (k == 0)
                song.sp[0][SP_HARM] = 2, song.sp[0][SP_FOLD] = 60;
            engine_init(RATE);
            engine_set_playing(1);
            if (k == 1)
                engine_express_param(0, SP_HARM, 1, 2), engine_express_param(0, SP_FOLD, 1, 60);
            engine_render(pcm, RATE);
            e[k][0] = 0, e[k][1] = live.meter[0];
            for (int i = 0; i < RATE * 2; i++)
                e[k][0] += (double)pcm[i] * pcm[i];
            engine_express_param(0, SP_HARM, 0, 0), engine_express_param(0, SP_FOLD, 0, 0);
        }
        int song_kept = song.sp[0][SP_HARM] == 0 && song.sp[0][SP_FOLD] == 0;
        printf("stick control: HARM 3RD and FOLD 60 played live as from the song %d (energy %.4g / %.4g), song kept %d\n",
               fabs(e[0][0] - e[1][0]) <= 1e-9 * e[0][0], e[0][0], e[1][0], song_kept);

        /* the lights: written as colours and given back at the end */
        char ld[600], lp[700];
        snprintf(ld, sizeof ld, "%s/led_anim", out_dir);
        char mk[700];
        snprintf(mk, sizeof mk, "mkdir -p '%s'", ld);
        if (system(mk) != 0)
            printf("lights: no folder\n");
        /* as MustardOS leaves them: brightness 0 */
        const char *files[8][2] = {{"effect_rgb_hex_l", "00FF00 "}, {"effect_rgb_hex_r", "0000FF "},
                                   {"effect_l", "2"},           {"effect_r", "2"},
                                   {"max_scale", "0"},          {"effect_enable", "0"},
                                   {"effect_cycles_l", "3"},    {"effect_cycles_r", "3"}};
        for (int i = 0; i < 8; i++) {
            snprintf(lp, sizeof lp, "%s/%s", ld, files[i][0]);
            FILE *f = fopen(lp, "w");
            if (f)
                fprintf(f, "%s\n", files[i][1]), fclose(f);
        }
        int found = leds_open(ld);
        leds_show(stick_led_colour(0xff8018, 1.f), stick_led_colour(0x18e8a0, 0.5f));
        char got[8][32];
#define LED_READ(n)                                                                                                    \
    for (int i = 0; i < (n); i++) {                                                                                    \
        snprintf(lp, sizeof lp, "%s/%s", ld, files[i][0]);                                                             \
        FILE *f = fopen(lp, "r");                                                                                      \
        got[i][0] = 0;                                                                                                 \
        if (f) {                                                                                                       \
            if (!fgets(got[i], sizeof got[i], f))                                                                      \
                got[i][0] = 0;                                                                                         \
            fclose(f);                                                                                                 \
        }                                                                                                              \
        got[i][strcspn(got[i], " \n")] = 0;                                                                            \
    }
        LED_READ(8)
        int shown_ok = !strcmp(got[0], "FF8018") && !strcmp(got[2], "4") && !strcmp(got[3], "4") &&
                       !strcmp(got[4], "60") && !strcmp(got[5], "1") && !strcmp(got[6], "-1");
        printf("lights: found %d, left %s right %s effect %s/%s brightness %s enable %s cycles %s (want FF8018 and a "
               "dimmer green, effect 4, brightness 60, enable 1, cycles -1: %d)",
               found, got[0], got[1], got[2], got[3], got[4], got[5], got[6], shown_ok);
        leds_close();
        LED_READ(8)
#undef LED_READ
        printf("; given back %s %s brightness %s cycles %s (want 00FF00 0000FF 0 3: %d)\n", got[0], got[1], got[4],
               got[6], !strcmp(got[0], "00FF00") && !strcmp(got[1], "0000FF") && !strcmp(got[4], "0") &&
                   !strcmp(got[6], "3"));
    }

    { /* the Brick Pro's rings, LED by LED (frame_hex): the left stick up lights the left ring's upper corners,
       * the right stick right the right ring's right corners; the rest dark; given back at the end */
        char ld[600], lp[700], mk[700];
        snprintf(ld, sizeof ld, "%s/led_frame", out_dir);
        snprintf(mk, sizeof mk, "mkdir -p '%s'", ld);
        if (system(mk) != 0)
            printf("rings: no folder\n");
        const char *files[][2] = {{"effect_rgb_hex_l", "000000 "}, {"effect_rgb_hex_r", "000000 "}, {"effect_l", "4"},
                                  {"effect_r", "4"}, {"effect_m", "4"}, {"max_scale", "0"}, {"effect_enable", "1"},
                                  {"frame_hex", "000000 "}};
        for (int i = 0; i < 8; i++) {
            snprintf(lp, sizeof lp, "%s/%s", ld, files[i][0]);
            FILE *f = fopen(lp, "w");
            if (f)
                fprintf(f, "%s\n", files[i][1]), fclose(f);
        }
        setenv("JD_LED_FRAME", "1", 1);
        int found = leds_open(ld);
        uint32_t l[4], r[4];
        stick_ring(0.f, 1.f, 0xff8018, l);
        stick_ring(1.f, 0.f, 0x18e8a0, r);
        leds_show_rings(l, r);
        char frame[400] = "", en[16] = "";
        snprintf(lp, sizeof lp, "%s/frame_hex", ld);
        FILE *f = fopen(lp, "r");
        if (f) {
            if (!fgets(frame, sizeof frame, f))
                frame[0] = 0;
            fclose(f);
        }
        snprintf(lp, sizeof lp, "%s/effect_enable", ld);
        if ((f = fopen(lp, "r"))) {
            if (!fgets(en, sizeof en, f))
                en[0] = 0;
            fclose(f);
        }
        unsigned v[23] = {0};
        int n = 0;
        for (char *q = frame; n < 23 && sscanf(q, "%6x", &v[n]) == 1; q += 7)
            n++;
        int lit[23];
        for (int i = 0; i < 23; i++)
            lit[i] = v[i] != 0;
        /* 11-14 the left ring (upper left, lower left, lower right, upper right), 15-18 the right */
        int ok = found && n == 23 && lit[10] && !lit[11] && !lit[12] && lit[13] && !lit[14] && !lit[15] && lit[16] &&
                 lit[17] && v[10] == 0xff8018 && v[16] == 0x18e8a0 && en[0] == '0';
        for (int i = 0; i < 23; i++)
            if (i < 10 || i > 17)
                ok &= !lit[i];
        leds_close();
        unsetenv("JD_LED_FRAME");
        snprintf(lp, sizeof lp, "%s/frame_hex", ld);
        if ((f = fopen(lp, "r"))) {
            if (!fgets(frame, sizeof frame, f))
                frame[0] = 0;
            fclose(f);
        }
        snprintf(lp, sizeof lp, "%s/effect_enable", ld);
        if ((f = fopen(lp, "r"))) {
            if (!fgets(en, sizeof en, f))
                en[0] = 0;
            fclose(f);
        }
        int back = strncmp(frame, "000000 000000", 13) == 0 && en[0] == '1';
        printf("rings: left up lights the left ring's upper corners, right right the right ring's right corners, the "
               "rest dark: %d; given back (dark frame, effects on): %d\n", ok, back);
    }

    { /* the top bar's level meter: full scale on the left lights the left half to its red end, silence on the
       * right leaves the right half dark; -21 dB lights two LEDs from the centre */
        uint32_t b[8];
        level_bar(1.f, 0.f, b);
        int full = b[0] && b[1] && b[2] && b[3] && !b[4] && !b[5] && !b[6] && !b[7] && ((b[0] >> 16) & 255) > 200 &&
                   ((b[0] >> 8) & 255) < 60;
        level_bar(0.0891f, 0.0891f, b);
        int half = b[3] && b[2] && !b[1] && !b[0] && b[4] && b[5] && !b[6] && !b[7];
        /* shown in the frame, LEDs 1-8 */
        char ld[600], lp[700], mk[700];
        snprintf(ld, sizeof ld, "%s/led_bar", out_dir);
        snprintf(mk, sizeof mk, "mkdir -p '%s' && cp '%s/led_frame/'* '%s/'", ld, out_dir, ld);
        int shown = 0;
        if (system(mk) == 0) {
            setenv("JD_LED_FRAME", "1", 1);
            leds_open(ld);
            uint32_t z[4] = {0, 0, 0, 0};
            level_bar(1.f, 1.f, b);
            leds_set_bar(b);
            leds_show_rings(z, z);
            char frame[400] = "";
            snprintf(lp, sizeof lp, "%s/frame_hex", ld);
            FILE *f = fopen(lp, "r");
            if (f) {
                if (!fgets(frame, sizeof frame, f))
                    frame[0] = 0;
                fclose(f);
            }
            unsigned v[23] = {0};
            int n = 0;
            for (char *q = frame; n < 23 && sscanf(q, "%6x", &v[n]) == 1; q += 7)
                n++;
            shown = n == 23;
            for (int i = 0; i < 23; i++)
                shown &= i < 8 ? v[i] == b[i] : v[i] == 0;
            leds_set_bar(NULL);
            leds_close();
            unsetenv("JD_LED_FRAME");
        }
        printf("level bar: full scale left %d, -21 dB two from the centre %d, shown on LEDs 1-8 %d\n", full, half,
               shown);
    }

    /* DSP benchmark: 30 s of the default drone */
    song_default();
    engine_init(RATE);
    engine_set_playing(1);
    clock_t c0 = clock();
    for (int i = 0; i < 30 * RATE / 1024; i++)
        engine_render(pcm, 1024);
    double secs_cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("dsp: 30 s rendered in %.2f s host cpu (%.1f%%)\n", secs_cpu, secs_cpu / 30.0 * 100.0);
    song_default();
    for (int s = 0; s < NS; s++)
        song.sp[s][SP_VOICE] = 1 + EX_BANSURI;
    song.gp[GP_SUB] = 50;
    engine_init(RATE);
    engine_set_playing(1);
    c0 = clock();
    for (int i = 0; i < 30 * RATE / 1024; i++)
        engine_render(pcm, 1024);
    secs_cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("dsp WIND (bansuri, undertones): 30 s in %.2f s host cpu (%.1f%%)\n", secs_cpu, secs_cpu / 30.0 * 100.0);
    song_default(); /* the heaviest struck case: five bowed cymbals */
    for (int s = 0; s < NS; s++)
        song.sp[s][SP_VOICE] = VOICE_PERC(PC_CYMBAL), song.sp[s][SP_BOW] = 60;
    engine_init(RATE);
    engine_set_playing(1);
    c0 = clock();
    for (int i = 0; i < 30 * RATE / 1024; i++)
        engine_render(pcm, 1024);
    secs_cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
    printf("dsp STRUCK (five bowed cymbals): 30 s in %.2f s host cpu (%.1f%%)\n", secs_cpu, secs_cpu / 30.0 * 100.0);
    /* the worst cases: every effect at its top (the sympathetic strings,
     * body, delay, reverb, binaural, undertones), each voice at full bow or
     * breath, ring, sizzle, fold and stretch, the fastest tempo: five bowed
     * cymbals, then a mixture of struck, wind and string. Everyday playing
     * costs less than these. */
    {
        static const int mixes[3][NS] = {
            {VOICE_PERC(PC_CYMBAL), VOICE_PERC(PC_CYMBAL), VOICE_PERC(PC_CYMBAL), VOICE_PERC(PC_CYMBAL),
             VOICE_PERC(PC_CYMBAL)},
            {VOICE_PERC(PC_CYMBAL), VOICE_PERC(PC_CYMBAL), 1 + EX_FLUTE, 1 + EX_FLUTE, 1 + EX_FREE},
            {0, 1 + EX_FREE, VOICE_PERC(PC_CYMBAL), 1 + EX_FLUTE, VOICE_PERC(PC_GONG)},
        };
        static const char *const mix_names[3] = {"five bowed cymbals", "2 cymbals, 2 flutes, free reed",
                                                 "string, free reed, cymbal, flute, gong"};
        for (int m = 0; m < 3; m++) {
            song_default();
            for (int p = 0; p < NFX; p++)
                song.fx[p] = fparam_def(p)->max;
            song.gp[GP_SUB] = gparam_def(GP_SUB)->max;
            song.gp[GP_SUBRES] = gparam_def(GP_SUBRES)->max;
            song.gp[GP_BPM] = gparam_def(GP_BPM)->max;
            for (int s = 0; s < NS; s++) {
                song.on[s] = 1;
                song.sp[s][SP_VOICE] = mixes[m][s];
                static const int top[] = {SP_BOW, SP_DECAY, SP_BUZZ, SP_FOLD, SP_STIFF, SP_BRIGHT};
                for (int k = 0; k < 6; k++)
                    song.sp[s][top[k]] = sparam_def(top[k])->max;
            }
            engine_init(RATE);
            engine_set_playing(1);
            c0 = clock();
            for (int i = 0; i < 30 * RATE / 1024; i++)
                engine_render(pcm, 1024);
            secs_cpu = (double)(clock() - c0) / CLOCKS_PER_SEC;
            printf("dsp WORST (%s, everything up): 30 s in %.2f s host cpu (%.1f%%), sounding %d\n", mix_names[m],
                   secs_cpu, secs_cpu / 30.0 * 100.0, rms_of(pcm, 0, 1024) > 100);
        }
    }

    /* the struck modes kept between sub-blocks give exactly the sound of
     * working them out every time: with drift moving the pitch, CHORD
     * following the scale's changes, and a voice switched mid-way */
    {
        static int16_t a[RATE * 6 * 2], b[RATE * 6 * 2];
        for (int pass = 0; pass < 2; pass++) {
            song_default();
            song.gp[GP_DRIFT] = gparam_def(GP_DRIFT)->max;
            song.gp[GP_CHANGES] = 1;
            for (int s = 0; s < NS; s++)
                song.sp[s][SP_VOICE] = VOICE_PERC(s == 4 ? PC_CHORD : s * 2), song.sp[s][SP_STIFF] = 30 * (s & 1);
            engine_perc_cache = pass;
            engine_init(RATE);
            engine_set_playing(1);
            int16_t *dst = pass ? b : a;
            for (int i = 0; i < 6; i++) {
                if (i % 2 == 0)
                    engine_pluck(0), engine_pluck(2), engine_pluck(4);
                if (i == 3)
                    song.sp[1][SP_VOICE] = VOICE_PERC(PC_CYMBAL);
                engine_render(dst + 2 * RATE * i, RATE);
            }
        }
        engine_perc_cache = 1;
        printf("struck cache: same sound as working the modes out every time %d\n", memcmp(a, b, sizeof a) == 0);
    }

    /* AIR in LATCH: a latched flute, four gongs listening. The listeners are
     * never latched (pressing them does nothing) yet sing; with AIR off they
     * stay silent */
    {
        extern float engine_vout_pk[NS];
        float sing[2];
        int refused = 0;
        for (int a = 0; a < 2; a++) {
            song_default();
            song.gp[GP_PLAYMODE] = PLAY_LATCH;
            song.gp[GP_CHANGES] = 0;
            song.fx[FX_AIR] = a ? 100 : 0;
            song.sp[0][SP_VOICE] = 1 + EX_FLUTE;
            for (int v = 1; v < NS; v++)
                song.sp[v][SP_VOICE] = VOICE_PERC(PC_GONG), song.sp[v][SP_LISTEN] = AIR_LISTENS,
                song.sp[v][SP_NOTE] = song.sp[0][SP_NOTE], song.sp[v][SP_DECAY] = 100;
            engine_init(RATE);
            engine_set_playing(1);
            engine_render(pcm, 1024);
            engine_latch(0, LATCH_BOW);
            refused += engine_latch(2, LATCH_BOW) == LATCH_OFF;
            engine_render(pcm, RATE * 4);
            memset(engine_vout_pk, 0, sizeof engine_vout_pk);
            engine_render(pcm, RATE);
            sing[a] = 0;
            for (int v = 1; v < NS; v++)
                sing[a] += engine_vout_pk[v];
        }
        printf("air in latch: listeners off %.4f, on %.3f (want them singing: %d), a listener refuses the latch %d\n",
               sing[0], sing[1], sing[1] > 0.02f && sing[0] < 1e-4f, refused == 2);
    }

    /* skipping silent struck modes: the same sound with and without, and
     * cheaper once a struck body has rung down (no bow, struck now and then,
     * strings ringing beside it into the sympathetic strings) */
    {
        static int16_t a[RATE * 12 * 2], b[RATE * 12 * 2];
        double cpu[2];
        for (int pass = 0; pass < 2; pass++) {
            song_default();
            song.fx[FX_SYMP] = fparam_def(FX_SYMP)->max;
            song.fx[FX_SCOUNT] = fparam_def(FX_SCOUNT)->max;
            for (int s = 0; s < NS; s++)
                song.sp[s][SP_VOICE] = s < 3 ? VOICE_PERC(s == 0 ? PC_CYMBAL : s == 1 ? PC_GENDER : PC_DRUM) : 0,
                song.sp[s][SP_BOW] = s < 3 ? 0 : song.sp[s][SP_BOW];
            engine_perc_skip = pass;
            engine_init(RATE);
            engine_set_playing(1);
            int16_t *dst = pass ? b : a;
            clock_t t0 = clock();
            for (int i = 0; i < 12; i++) {
                if (i % 4 == 0)
                    engine_pluck(0), engine_pluck(1), engine_pluck(2);
                engine_render(dst + 2 * RATE * i, RATE);
            }
            cpu[pass] = (double)(clock() - t0) / CLOCKS_PER_SEC;
        }
        engine_perc_skip = 1;
        double sig = 0, err = 0;
        for (int i = 0; i < RATE * 12 * 2; i++) {
            double d = (double)a[i] - b[i];
            sig += (double)a[i] * a[i];
            err += d * d;
        }
        double snr = err > 0 ? 10 * log10(sig / err) : 200;
        /* 60 dB: inaudible. (Out of the limiter's reach it is 87 dB; the limiter holds its
         * gain for 80 ms, which carries skipping's rounding a little further.) */
        printf("struck skipping: difference %.0f dB below the sound (want over 60: %d); cpu %.2f s -> %.2f s\n", snr,
               snr > 60, cpu[0], cpu[1]);
    }
    song_default();
    for (int s = 0; s < NS; s++)
        song.sp[s][SP_VOICE] = 1 + EX_BANSURI;
    engine_init(RATE);
    engine_set_playing(1);
    /* mixed screens: two strings, a reed, a gamelan gender and a singing bowl */
    song.sp[0][SP_VOICE] = song.sp[1][SP_VOICE] = 0;
    song.sp[2][SP_VOICE] = 1 + EX_REED;
    song.sp[3][SP_VOICE] = VOICE_PERC(PC_GENDER);
    song.sp[4][SP_VOICE] = VOICE_PERC(PC_BOWL);
    for (int i = 0; i < 300; i++) {
        engine_render(pcm, RATE / 100);
        ui_tick(10);
    }
    shot(out, "wind_play", fb);
    next_page();
    shot(out, "wind_sound", fb);
    {
        /* SONG with a progression recorded and two presets */
        engine_prog_mode(PROG_RECORD);
        for (int i = 0; i < 5; i++)
            engine_next_chord();
        engine_prog_mode(PROG_PLAY);
        engine_next_chord();
        engine_next_chord();
        char pp[512];
        snprintf(pp, sizeof pp, "%s/preset_01.jds", out);
        song_save(pp);
        for (int i = 0; i < PAGE_COUNT - 2; i++)
            next_page(); /* SOUND -> SONG */
        ui_set_dir(out);
        shot(out, "song", fb);
        next_page(); /* back to PLAY */
        next_page();
    }
    for (int i = 0; i < PAGE_COUNT - 1; i++)
        next_page();
    snprintf(path, sizeof path, "%s/wind.wav", out);
    {
        song_default();
        for (int s = 0; s < NS; s++)
            song.sp[s][SP_VOICE] = 1 + EX_BANSURI;
        song.gp[GP_SUB] = 40;
        engine_init(RATE);
        engine_set_playing(1);
        engine_render(pcm, RATE * 12);
        write_wav(path, pcm, RATE * 12);
    }
    free(pcm);
    return 0;
}
