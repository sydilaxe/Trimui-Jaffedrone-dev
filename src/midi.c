/* MIDI over USB (see midi.h). Everything here runs in the MIDI thread
 * except midi_take_notice; changes to the sound go through ui_lock, as the
 * buttons' do. */
#define _GNU_SOURCE
#include "midi.h"
#include "engine.h"
#include "ui.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------ settings */

enum { NOTES_POLY, NOTES_CHANNELS };
static int notes_mode = NOTES_POLY, clock_out = 1, follow = 1;
static volatile float latency_s = 0.03f;

void midi_set_latency(float seconds) { latency_s = seconds < 0.f ? 0.f : seconds; }

/* ------------------------------------------------------------- notices */

static pthread_mutex_t nmx = PTHREAD_MUTEX_INITIALIZER;
static char notices[8][48];
static int n_head, n_tail;

static void notice(const char *fmt, const char *arg)
{
    pthread_mutex_lock(&nmx);
    if (n_head - n_tail < 8) {
        snprintf(notices[n_head % 8], sizeof notices[0], fmt, arg);
        n_head++;
    }
    pthread_mutex_unlock(&nmx);
}

int midi_take_notice(char *buf, int n)
{
    int got = 0;
    pthread_mutex_lock(&nmx);
    if (n_tail < n_head) {
        snprintf(buf, (size_t)n, "%s", notices[n_tail % 8]);
        n_tail++;
        got = 1;
    }
    pthread_mutex_unlock(&nmx);
    return got;
}

/* ---------------------------------------------------------- the notes */

typedef struct {
    int note, ch, vel; /* the key it plays (note -1: none) */
    int held, sustained;
    uint64_t on_us, off_us;
} Hold;
static Hold hold[NS];
static float pb[16];     /* pitch bend, semitones */
static int sustain[16];
static float wheel;      /* CC1, 0 .. 1 */
static float press[16];  /* channel pressure, 0 .. 1 (-1: none yet) */

/* engine_position read without the engine's lock: two variables, so a read
 * may catch them mid-change; the middle of three is always whole */
static double position(void)
{
    double a = engine_position(), b = engine_position(), c = engine_position();
    if (a > b) {
        double t = a;
        a = b, b = t;
    }
    return c < a ? a : c > b ? b : c;
}

static void retune(int s)
{
    Hold *h = &hold[s];
    if (h->note < 0)
        return;
    float bend = h->note + pb[h->ch] - engine_string_pitch(s);
    while (bend > 24.f) /* out of reach: the nearest octave of it */
        bend -= 12.f;
    while (bend < -24.f)
        bend += 12.f;
    engine_express_bend(s, bend);
}

static float pressure(const Hold *h)
{
    float p = press[h->ch] >= 0.f ? 0.2f + 1.1f * press[h->ch] : 0.3f + 0.9f * h->vel / 127.f;
    return p * (1.f + 0.5f * wheel);
}

static int pick_string(int ch, int note)
{
    for (int s = 0; s < NS; s++) /* the key again: the same string */
        if (hold[s].note == note && hold[s].ch == ch && hold[s].held)
            return s;
    if (notes_mode == NOTES_CHANNELS)
        return ch < NS ? ch : ui_selected_string();
    int best = -1;
    uint64_t oldest = UINT64_MAX;
    for (int s = 0; s < NS; s++) /* free the longest, playing or not */
        if (!hold[s].held && song.on[s] && hold[s].off_us < oldest)
            oldest = hold[s].off_us, best = s;
    if (best >= 0)
        return best;
    for (int s = 0; s < NS; s++) /* all held: the one held longest */
        if (hold[s].on_us < oldest)
            oldest = hold[s].on_us, best = s;
    return best;
}

static void let_go(int s, uint64_t t)
{
    Hold *h = &hold[s];
    if (!h->held)
        return;
    h->held = h->sustained = 0;
    h->off_us = t;
    if (song.gp[GP_PLAYMODE] == PLAY_LATCH) {
        if (live.latch[s] != LATCH_OFF)
            engine_latch(s, LATCH_OFF); /* played again: it fades over its RELEASE */
    }
    else {
        engine_bow(s, 0);
        engine_bow_pressure(s, 1.f);
    }
}

static void note_on(int ch, int note, int vel, uint64_t t)
{
    int s = pick_string(ch, note);
    if (s < 0)
        return;
    Hold *h = &hold[s];
    int again = h->held;
    if (h->held && (h->note != note || h->ch != ch))
        let_go(s, t); /* taken from another key */
    h->note = note, h->ch = ch, h->vel = vel;
    h->held = 1, h->sustained = 0;
    h->on_us = t;
    retune(s);
    if (!live.playing)
        engine_set_playing(1);
    if (song.gp[GP_PLAYMODE] == PLAY_LATCH) {
        if (live.latch[s] == LATCH_OFF) /* a soft key swells in with the bow, a firm one plucks */
            engine_latch(s, vel <= 48 ? LATCH_BOW : LATCH_PLUCK);
        else if (again)
            engine_pluck_amp(s, vel / 127.f);
    }
    else {
        engine_pluck_amp(s, 0.25f + 0.75f * vel / 127.f);
        engine_bow_pressure(s, pressure(h));
        engine_bow(s, 1);
    }
}

static void note_off(int ch, int note, uint64_t t)
{
    for (int s = 0; s < NS; s++) {
        Hold *h = &hold[s];
        if (h->held && h->note == note && h->ch == ch) {
            if (sustain[ch])
                h->sustained = 1;
            else
                let_go(s, t);
        }
    }
}

static void all_off(uint64_t t)
{
    for (int s = 0; s < NS; s++)
        let_go(s, t);
}

static void control(int ch, int cc, int v, uint64_t t)
{
    switch (cc) {
    case 1:
        wheel = v / 127.f;
        for (int s = 0; s < NS; s++)
            if (hold[s].held && song.gp[GP_PLAYMODE] != PLAY_LATCH)
                engine_bow_pressure(s, pressure(&hold[s]));
        break;
    case 64:
        sustain[ch] = v >= 64;
        if (!sustain[ch])
            for (int s = 0; s < NS; s++)
                if (hold[s].sustained && hold[s].ch == ch)
                    let_go(s, t);
        break;
    case 74: engine_express_tone((v - 64) / 64.f * 2.f); break;
    case 91: engine_express_space((v - 64) / 63.f); break;
    case 120:
    case 123: all_off(t); break;
    }
}

/* ------------------------------------------------------------ clock in */

static uint64_t tick_us, ext_seen_us;
static double tick_avg_us;
static int64_t ticks;     /* since START (or the song position) */
static int following;     /* an outside clock leads */
static int ext_running;

static void clock_tick(uint64_t t)
{
    if (tick_us && t > tick_us && t - tick_us < 250000) {
        double dt = (double)(t - tick_us);
        tick_avg_us = tick_avg_us > 0.0 ? tick_avg_us + (dt - tick_avg_us) * 0.08 : dt;
    }
    tick_us = t;
    ext_seen_us = t;
    if (!follow || tick_avg_us <= 0.0)
        return;
    following = 1;
    float bpm = (float)(60e6 / (tick_avg_us * 24.0));
    if (fabsf(bpm - engine_bpm()) > 0.05f)
        engine_set_bpm(bpm);
    if (!ext_running || !live.playing)
        return;
    ticks++;
    /* the phase: the engine runs ahead of the sound by the render-ahead time,
     * so it should stand that far past the clock; trimmed gently, jumped
     * only when far off */
    double want = ticks / 6.0 + latency_s * engine_bpm() / 15.0, err = want - position();
    if (fabs(err) > 2.0) {
        engine_set_position(want);
        engine_set_rate(1.0);
    }
    else
        engine_set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.05)));
}

static void realtime(uint8_t b, uint64_t t)
{
    switch (b) {
    case 0xf8: clock_tick(t); break;
    case 0xfa: /* START: from the top */
        if (!follow)
            break;
        ticks = 0;
        ext_running = 1;
        engine_set_position(latency_s * engine_bpm() / 15.0);
        engine_set_playing(1);
        break;
    case 0xfb: /* CONTINUE */
        if (!follow)
            break;
        ext_running = 1;
        engine_set_playing(1);
        break;
    case 0xfc: /* STOP */
        if (!follow)
            break;
        ext_running = 0;
        engine_set_rate(1.0);
        engine_set_playing(0);
        break;
    }
}

/* ------------------------------------------------------------- parsing */

static uint8_t status, data[2];
static int ndata, in_sysex;

static int data_len(uint8_t st)
{
    switch (st & 0xf0) {
    case 0xc0:
    case 0xd0: return 1;
    case 0xf0:
        return st == 0xf2 ? 2 : st == 0xf1 || st == 0xf3 ? 1 : 0;
    default: return 2;
    }
}

static void message(uint64_t t)
{
    int ch = status & 15;
    switch (status & 0xf0) {
    case 0x90:
        if (data[1]) {
            note_on(ch, data[0], data[1], t);
            break;
        }
        /* velocity 0 is a release */
        __attribute__((fallthrough));
    case 0x80: note_off(ch, data[0], t); break;
    case 0xb0: control(ch, data[0], data[1], t); break;
    case 0xd0:
        press[ch] = data[0] / 127.f;
        for (int s = 0; s < NS; s++)
            if (hold[s].held && hold[s].ch == ch && song.gp[GP_PLAYMODE] != PLAY_LATCH)
                engine_bow_pressure(s, pressure(&hold[s]));
        break;
    case 0xe0:
        pb[ch] = (((data[1] << 7) | data[0]) - 8192) / 8192.f * 2.f;
        for (int s = 0; s < NS; s++)
            if (hold[s].held && hold[s].ch == ch)
                retune(s);
        break;
    case 0xf0:
        if (status == 0xf2 && follow) { /* song position, in 16ths */
            ticks = (int64_t)((data[1] << 7) | data[0]) * 6;
            engine_set_position(ticks / 6.0 + latency_s * engine_bpm() / 15.0);
        }
        break;
    }
}

static void feed(const uint8_t *b, int n, uint64_t t)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xf8) { /* real time: anywhere, even inside another message */
            realtime(c, t);
            continue;
        }
        if (c == 0xf0) {
            in_sysex = 1;
            continue;
        }
        if (c == 0xf7) {
            in_sysex = 0;
            continue;
        }
        if (c & 0x80) {
            in_sysex = 0;
            status = c;
            ndata = 0;
            if (data_len(c) == 0) {
                message(t);
                status = 0;
            }
            continue;
        }
        if (in_sysex || !status)
            continue;
        data[ndata++] = c;
        if (ndata >= data_len(status)) {
            message(t);
            ndata = 0; /* running status: the same kind again */
            if (status >= 0xf0)
                status = 0;
        }
    }
}

void midi_feed(const uint8_t *b, int n, uint64_t t)
{
    ui_lock();
    feed(b, n, t);
    ui_unlock();
}

void midi_reset(void)
{
    for (int s = 0; s < NS; s++) {
        memset(&hold[s], 0, sizeof hold[s]);
        hold[s].note = -1;
        engine_express_bend(s, 0.f);
    }
    for (int c = 0; c < 16; c++)
        pb[c] = 0.f, sustain[c] = 0, press[c] = -1.f;
    wheel = 0.f;
    tick_us = ext_seen_us = 0;
    tick_avg_us = 0.0;
    ticks = 0;
    following = ext_running = 0;
    status = 0, ndata = 0, in_sysex = 0;
}

/* ------------------------------------------------------------ clock out */

static int out_running;
static double out_est;
static int64_t out_sent;
static uint64_t out_t;

int midi_clock_step(uint64_t t, uint8_t *out, int max)
{
    int n = 0;
    int play = live.playing && clock_out && !following;
    if (!play) {
        if (out_running && n < max)
            out[n++] = 0xfc;
        out_running = 0;
        return n;
    }
    double bpm = engine_bpm();
    double heard = position() - latency_s * bpm / 15.0;
    if (!out_running) { /* START now: the next tick is the first */
        out_running = 1;
        out_est = heard;
        out_sent = (int64_t)floor(heard * 6.0);
        out_t = t;
        if (n < max)
            out[n++] = 0xfa;
        return n;
    }
    /* the engine's position moves in blocks: a smooth estimate between them,
     * held to it */
    out_est += (double)(t - out_t) * 1e-6 * bpm / 15.0;
    out_t = t;
    double d = heard - out_est;
    out_est += fabs(d) > 1.0 ? d : d * 0.02;
    int64_t want = (int64_t)floor(out_est * 6.0);
    if (want - out_sent > 4) /* a jump (a new position): no burst of ticks */
        out_sent = want - 1;
    while (out_sent < want && n < max) {
        out[n++] = 0xf8;
        out_sent++;
    }
    if (want < out_sent)
        out_sent = want; /* moved back: wait for it */
    return n;
}

/* --------------------------------------------------------------- ports */

#define MAXPORT 8
typedef struct {
    int fd;
    char path[40], name[32];
} Port;
static Port ports[MAXPORT];
static int nports;

static void port_name(int card, char *b, int n)
{
    char p[64];
    snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
    FILE *f = fopen(p, "r");
    b[0] = 0;
    if (f) {
        if (fgets(b, n, f))
            b[strcspn(b, "\n")] = 0;
        fclose(f);
    }
    if (!b[0])
        snprintf(b, (size_t)n, "card %d", card);
}

static void scan(void)
{
    DIR *d = opendir("/dev/snd");
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)) && nports < MAXPORT) {
        int card, devn;
        if (sscanf(e->d_name, "midiC%dD%d", &card, &devn) != 2)
            continue;
        char path[40];
        snprintf(path, sizeof path, "/dev/snd/%.30s", e->d_name);
        int known = 0;
        for (int i = 0; i < nports; i++)
            known |= !strcmp(ports[i].path, path);
        if (known)
            continue;
        int fd = open(path, O_RDWR | O_NONBLOCK);
        if (fd < 0)
            fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0)
            continue;
        Port *p = &ports[nports++];
        p->fd = fd;
        snprintf(p->path, sizeof p->path, "%s", path);
        port_name(card, p->name, sizeof p->name);
        notice("MIDI: %.40s", p->name);
    }
    closedir(d);
}

static void drop(int i)
{
    notice("MIDI: %.34s GONE", ports[i].name);
    close(ports[i].fd);
    ports[i] = ports[--nports];
}

static void send_all(const uint8_t *b, int n)
{
    for (int i = 0; i < nports; i++)
        if (write(ports[i].fd, b, (size_t)n) < 0) {
            /* a full or vanished port: a port that went away is dropped when read */
        }
}

/* ------------------------------------------------------------- thread */

static pthread_t th;
static volatile int running;
static int core_;

static void *loop(void *arg)
{
    (void)arg;
    if (core_ >= 0) {
        cpu_set_t cs;
        CPU_ZERO(&cs);
        CPU_SET(core_, &cs);
        pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
    }
    uint64_t next_scan = 0, next_tune = 0;
    while (running) {
        uint64_t t = now_us();
        if (t >= next_scan) {
            scan();
            next_scan = t + 1000000;
        }
        struct pollfd pf[MAXPORT];
        for (int i = 0; i < nports; i++)
            pf[i].fd = ports[i].fd, pf[i].events = POLLIN, pf[i].revents = 0;
        poll(pf, (nfds_t)nports, 1);
        t = now_us();
        for (int i = nports - 1; i >= 0; i--) {
            if (!(pf[i].revents & (POLLIN | POLLERR | POLLHUP)))
                continue;
            uint8_t buf[256];
            ssize_t r = read(ports[i].fd, buf, sizeof buf);
            if (r > 0)
                midi_feed(buf, (int)r, t);
            else if (r == 0 || (errno != EAGAIN && errno != EINTR))
                drop(i);
        }
        int stale = following && t - ext_seen_us > 1000000;
        if (!stale && t < next_tune)
            goto clock;
        ui_lock();
        if (stale) { /* the outside clock has stopped */
            following = ext_running = 0;
            engine_set_rate(1.0);
        }
        if (t >= next_tune) { /* chords move the strings: the held keys stay in tune */
            for (int s = 0; s < NS; s++) {
                if (hold[s].held)
                    retune(s);
                else if (hold[s].note >= 0 && t - hold[s].off_us > 4000000) {
                    hold[s].note = -1; /* rung out: back to its own note */
                    engine_express_bend(s, 0.f);
                }
            }
            next_tune = t + 20000;
        }
        ui_unlock();
    clock:;
        uint8_t out[16];
        int n = midi_clock_step(t, out, sizeof out);
        if (n)
            send_all(out, n);
    }
    uint8_t stop = 0xfc;
    if (out_running)
        send_all(&stop, 1);
    for (int i = 0; i < nports; i++)
        close(ports[i].fd);
    nports = 0;
    return NULL;
}

void midi_start(int core)
{
    const char *v = getenv("JD_MIDI_NOTES");
    notes_mode = v && !strcasecmp(v, "channels") ? NOTES_CHANNELS : NOTES_POLY;
    v = getenv("JD_MIDI_CLOCK");
    clock_out = !(v && *v == '0');
    v = getenv("JD_MIDI_FOLLOW");
    follow = !(v && *v == '0');
    midi_reset();
    core_ = core;
    running = 1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    struct sched_param sp = {.sched_priority = 40}; /* below the sound, above the screen */
    pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&a, SCHED_FIFO);
    pthread_attr_setschedparam(&a, &sp);
    if (pthread_create(&th, &a, loop, NULL) != 0 && pthread_create(&th, NULL, loop, NULL) != 0)
        running = 0;
    pthread_attr_destroy(&a);
}

void midi_stop(void)
{
    if (!running)
        return;
    running = 0;
    pthread_join(th, NULL);
}
