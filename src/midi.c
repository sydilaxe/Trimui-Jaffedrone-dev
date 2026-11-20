/* MIDI over USB (see midi.h), on brickio (lib/brickio): its reader finds and
 * reads the ports, its musical clock (mclock, wired by mclockio) follows, leads
 * and passes on the clock over MIDI and the Brick link. What is Jaffedrone's
 * own is here: the keys, their expression, the CCs, the MIDI page's clock
 * settings, the notices. The keys' changes to the sound go through ui_lock, as
 * the buttons' do. */
#define _GNU_SOURCE
#include "midi.h"
#include "engine.h"
#include "../lib/brickio/link.h"
#include "../lib/brickio/linkpos.h"
#include "../lib/brickio/mclockio.h"
#include "../lib/brickio/midi.h"
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
#include <sys/ioctl.h>
#include <linux/usbdevice_fs.h>
#include <time.h>
#include <unistd.h>


/* ------------------------------------------------------------ settings */

static int clock_out = 1, follow = 1, relay = 1;

static volatile float latency_s = 0.03f;
void midi_set_latency(float seconds)
{
    latency_s = seconds < 0.f ? 0.f : seconds;
    mclock_set_latency(latency_s);
}

/* the MIDI page's MIDI CLOCK switches over launch.sh's JD_MIDI_CLOCK / JD_MIDI_FOLLOW */
static int can_follow(void) { return follow && (song.gp[GP_MIDI] == MIDI_AUTO || song.gp[GP_MIDI] == MIDI_FOLLOW); }
static int can_send(void) { return clock_out && (song.gp[GP_MIDI] == MIDI_AUTO || song.gp[GP_MIDI] == MIDI_SEND); }

/* the drone's 16ths a tick in moves it; a tick out moves the clock sent */
static double in_ratio(void) { return midi_clkdiv_ratio(midi_cfg.in_div); }
static double out_ratio(void) { return midi_clkdiv_ratio(midi_cfg.out_div); }

/* ------------------------------------------------------------- notices */

static pthread_mutex_t nmx = PTHREAD_MUTEX_INITIALIZER;
static char notices[8][48];
static int n_head, n_tail;

void midi_notice(const char *fmt, const char *arg)
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
    int note, ch, vel, src; /* the key it plays, and the voice it sounds like */
    int held, sustained;
    float poly; /* its own (polyphonic) aftertouch, 0 .. 1 (-1: none yet) */
    uint64_t on_us, off_us;
} Key;
static Key key[NKEY];   /* each on key voice NS + k */
static float pb[16];    /* pitch bend, semitones */
static int sustain[16];
static float modw[16];  /* CC1, 0 .. 1 */
static float press[16]; /* channel pressure, 0 .. 1 (-1: none yet) */
/* the most keys one voice plays at once: the MIDI page's POLY (1-4) */
static int per_voice(void) { return midi_cfg.poly < 1 ? 1 : midi_cfg.poly > 4 ? 4 : midi_cfg.poly; }
/* a key drones until let go (HOLD LATCH, or HOLD SONG in PLAY MODE LATCH) */
static int key_latches(void)
{
    return midi_cfg.hold == KH_LATCH || (midi_cfg.hold == KH_SONG && song.gp[GP_PLAYMODE] == PLAY_LATCH);
}

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

/* the velocity as heard: VELOCITY 100 as struck, 0 every key alike */
static int vel_heard(int vel)
{
    int v = (int)lrintf(100.f + midi_cfg.vel / 100.f * (vel - 100));
    return v < 1 ? 1 : v > 127 ? 127 : v;
}

/* a key's expression onto its voice: the bend, and what aftertouch and the
 * mod wheel move (their amounts add when they move the same thing) */
static void express(int k)
{
    Key *h = &key[k];
    int v = NS + k;
    float at = h->poly >= 0.f ? h->poly : press[h->ch] >= 0.f ? press[h->ch] : 0.f;
    float amt[NMT] = {0};
    amt[midi_cfg.at_to < NMT ? midi_cfg.at_to : 0] += at;
    amt[midi_cfg.mod_to < NMT ? midi_cfg.mod_to : 0] += modw[h->ch];
    for (int t = 0; t < NMT; t++)
        amt[t] = amt[t] > 1.f ? 1.f : amt[t];
    engine_express_bend(v, pb[h->ch] + 2.f * amt[MT_PITCH]);
    engine_key_vibrato(v, 0.6f * amt[MT_VIBRATO]);
    float base = 0.3f + 0.9f * vel_heard(h->vel) / 127.f; /* a harder key bows harder */
    engine_bow_pressure(v, base + (1.5f - base) * amt[MT_BOW]);
    static const int tp[4][2] = {{MT_TONE, SP_BRIGHT}, {MT_FOLD, SP_FOLD}, {MT_BUZZ, SP_BUZZ}, {MT_SWELL, SP_LEVEL}};
    for (int i = 0; i < 4; i++) {
        int t = tp[i][0], p = tp[i][1], b = engine_param_played(h->src, p);
        int on = t == MT_SWELL ? midi_cfg.at_to == t || midi_cfg.mod_to == t : amt[t] > 0.f;
        float val = t == MT_SWELL ? b * (0.35f + 0.65f * amt[t]) : b + (sparam_def(p)->max - b) * amt[t];
        engine_express_param(v, p, on, (int)lrintf(val));
    }
    engine_overblow(v, amt[MT_HARM] > 0.5f);
}

/* a key voice for a key: the same key again keeps its voice; a voice already
 * playing PER_VOICE keys gives its oldest up; else a silent key voice, the
 * one let go longest ago, or with all held, the key held longest */
static int pick_key(int ch, int note, int src)
{
    for (int k = 0; k < NKEY; k++)
        if (key[k].held && key[k].note == note && key[k].ch == ch)
            return k;
    int n = 0, best = -1;
    uint64_t oldest = UINT64_MAX;
    for (int k = 0; k < NKEY; k++)
        if (key[k].held && key[k].src == src) {
            n++;
            if (key[k].on_us < oldest)
                oldest = key[k].on_us, best = k;
        }
    if (n >= per_voice())
        return best;
    for (int k = 0; k < NKEY; k++)
        if (!key[k].held && !engine_key_sounding(NS + k))
            return k;
    best = -1, oldest = UINT64_MAX;
    for (int k = 0; k < NKEY; k++) /* still ringing: the one let go longest ago */
        if (!key[k].held && key[k].off_us < oldest)
            oldest = key[k].off_us, best = k;
    if (best >= 0)
        return best;
    for (int k = 0; k < NKEY; k++) /* all held: the one held longest */
        if (key[k].on_us < oldest)
            oldest = key[k].on_us, best = k;
    return best;
}

int midi_voice_of(int ch, int note)
{
    for (int k = 0; k < NKEY; k++)
        if (key[k].held && key[k].note == note && key[k].ch == ch)
            return NS + k;
    return -1;
}

static void let_go(int k, uint64_t t)
{
    Key *h = &key[k];
    int v = NS + k;
    if (!h->held)
        return;
    h->held = h->sustained = 0;
    h->off_us = t;
    if (engine_latch_state(v) != LATCH_OFF)
        engine_latch(v, LATCH_OFF); /* it fades over its RELEASE */
    else
        engine_bow(v, 0);
}

/* a key plays the voice its channel plays, at its own pitch; it never starts
 * the drone (only START, or a clock's START, does) */
static void note_on(int ch, int note, int vel, uint64_t t)
{
    int src = midi_channel_voice(ch);
    if (src < 0)
        return;
    if (midi_cfg.hold == KH_LATCH) /* HOLD LATCH: the droning key again lets it go */
        for (int k = 0; k < NKEY; k++)
            if (key[k].held && key[k].note == note && key[k].ch == ch) {
                let_go(k, t);
                return;
            }
    int k = pick_key(ch, note, src);
    if (k < 0)
        return;
    Key *h = &key[k];
    int v = NS + k, again = h->held && h->note == note && h->ch == ch;
    if (h->held && !again)
        let_go(k, t); /* taken from another key */
    h->note = note, h->ch = ch, h->vel = vel, h->src = src;
    h->held = 1, h->sustained = 0, h->poly = -1.f;
    h->on_us = t;
    /* its pitch: SHIFT semitones on, then SNAP to the song's scale */
    float pitch = (float)(note + midi_cfg.shift);
    if (midi_cfg.snap)
        pitch = engine_scale_snap(pitch);
    engine_key_voice(v, src, pitch);
    express(k);
    int vh = vel_heard(vel), pl = midi_cfg.play;
    float amp = 0.25f + 0.75f * vh / 127.f;
    if (key_latches()) {
        if (engine_latch_state(v) == LATCH_OFF) /* BOTH: a soft key swells in with the bow, a firm one plucks */
            engine_latch(v, pl == KP_BOW      ? LATCH_BOW
                            : pl == KP_STRIKE ? LATCH_HAMMER
                            : pl == KP_PLUCK  ? LATCH_PLUCK
                            : vh <= 48        ? LATCH_BOW
                                              : LATCH_PLUCK);
        else if (again)
            engine_pluck_amp(v, vh / 127.f);
    }
    else {
        if (pl == KP_STRIKE)
            engine_hammer_amp(v, amp);
        else if (pl != KP_BOW)
            engine_pluck_amp(v, amp);
        if (pl == KP_BOTH || pl == KP_BOW)
            engine_bow(v, 1);
    }
}

static void note_off(int ch, int note, uint64_t t)
{
    if (midi_cfg.hold == KH_LATCH) /* a latched key drones on: the key again lets it go */
        return;
    for (int k = 0; k < NKEY; k++) {
        Key *h = &key[k];
        if (h->held && h->note == note && h->ch == ch) {
            if (sustain[ch])
                h->sustained = 1;
            else
                let_go(k, t);
        }
    }
}

static void all_off(uint64_t t)
{
    for (int k = 0; k < NKEY; k++)
        let_go(k, t);
}

/* the keys of a channel, sounding: their expression again */
static void express_ch(int ch)
{
    for (int k = 0; k < NKEY; k++)
        if (key[k].ch == ch && key[k].note >= 0 && engine_key_sounding(NS + k))
            express(k);
}

/* the other CCs, for the main thread (ui_midi_cc): one writer (the MIDI
 * thread), one reader */
#define CCQ 256
static volatile uint32_t ccq[CCQ];
static volatile unsigned ccq_w, ccq_r;

int midi_take_cc(int *ch, int *cc, int *v)
{
    if (ccq_r == ccq_w)
        return 0;
    __sync_synchronize();
    uint32_t m = ccq[ccq_r % CCQ];
    __sync_synchronize();
    ccq_r++;
    *ch = (int)(m >> 16 & 15), *cc = (int)(m >> 8 & 127), *v = (int)(m & 127);
    return 1;
}

static void control(int ch, int cc, int v, uint64_t t)
{
    switch (cc) {
    case 1: /* the mod wheel: what the MIDI page sends it to */
        modw[ch] = v / 127.f;
        express_ch(ch);
        break;
    case 64:
        sustain[ch] = v >= 64;
        if (!sustain[ch])
            for (int k = 0; k < NKEY; k++)
                if (key[k].sustained && key[k].ch == ch)
                    let_go(k, t);
        break;
    case 74: engine_express_tone((v - 64) / 64.f * 2.f); break;
    case 91: engine_express_space((v - 64) / 63.f); break;
    case 120:
    case 123: all_off(t); break;
    default:
        if (ccq_w - ccq_r < CCQ) { /* full: the oldest are kept, a burst's last values dropped */
            ccq[ccq_w % CCQ] = (uint32_t)(ch & 15) << 16 | (uint32_t)(cc & 127) << 8 | (uint32_t)(v & 127);
            __sync_synchronize();
            ccq_w++;
        }
        break;
    }
}

/* ------------------------------------------------------------ the clock */

/* the MIDI page's MIDI CLOCK row (CLOCK IN and OUT, START IN, START OUT, IN DIV
 * and OUT DIV) over launch.sh's JD_MIDI_CLOCK / JD_MIDI_FOLLOW / JD_MIDI_RELAY,
 * into brickio's clock (a few atomics; any thread) */
static void apply(void)
{
    int f = can_follow(), s = can_send();
    mclock_set_mode(f && s ? MCLOCK_AUTO : f ? MCLOCK_FOLLOW : s ? MCLOCK_SEND : MCLOCK_OFF);
    mclock_set_transport(!song.gp[GP_MIDISTART]);
    mclock_set_start_out(midi_cfg.start_out);
    mclock_set_ratio(in_ratio(), out_ratio()); /* IN DIV 1/2: half time; OUT DIV x2: double */
    mclock_set_relay(relay && f && s); /* following on AUTO: the clock goes on to the other ports */
}

/* The drone, as brickio's clock sees it. mclockio calls the set_ functions
 * holding ui_lock, so they are plain calls. Started by a clock (or a leading
 * Brick), the drone starts where the sound being heard is the top, as it did */
static float h_bpm(void) { return engine_bpm(); }
static void h_set_bpm(float b) { engine_set_bpm(b); }
static void h_set_position(double s) { engine_set_position(s); }
static void h_set_rate(double r) { engine_set_rate(r); }
static int h_playing(void) { return live.playing; }
static void h_set_playing(int on)
{
    if (on && !live.playing)
        engine_set_position(latency_s * engine_bpm() / 15.0);
    engine_set_playing(on);
}
static const ClockHost host = {h_bpm, h_set_bpm, position, h_set_position, h_set_rate, h_playing, h_set_playing};

static void lk(void) { ui_lock(); }
static void ul(void) { ui_unlock(); }

/* brickio's Wi-Fi sync (HI5S) talks to the same drone from the main thread,
 * taking the audio lock for each change (and for the position, two variables) */
static int w_mode(void) { return song.gp[GP_SYNC]; }
static void w_set_bpm(float b) { lk(), engine_set_bpm(b), ul(); }
static double w_position(void) { lk(); double p = engine_position(); ul(); return p; }
static void w_set_position(double s) { lk(), engine_set_position(s), ul(); }
static void w_set_rate(double r) { lk(), engine_set_rate(r), ul(); }
static void w_set_playing(int on) { lk(), engine_set_playing(on), ul(); }
static const SyncClock wifi = {w_mode, h_bpm, w_set_bpm, w_position, w_set_position, w_set_rate, h_playing, w_set_playing};
const SyncClock *wifi_clock(void) { return &wifi; }
static void notice_line(const char *msg) { midi_notice("%.46s", msg); }

static int inited;
static void init_clock(int io)
{
    McioConfig c = {.host = &host, .lock = lk, .unlock = ul, .midi = io, .links = io, .notice = notice_line};
    mcio_init(&c);
    inited = 1;
}

int midi_following(void) { return mclock_following(); }
int midi_links(void) { return mcio_links(); }
int midi_ports(void) { return midi_count() + mcio_links(); }

/* ------------------------------------------------------------- parsing */

/* the keys, expression and CCs; the clock, song position and the link's
 * position are brickio's (the reader's clock hook, or mcio_feed) */
static uint8_t status, data[2];
static int ndata, skip; /* skip: inside a SysEx, or a system common message's data */

static int data_len(uint8_t st)
{
    switch (st & 0xf0) {
    case 0xc0:
    case 0xd0: return 1;
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
    case 0xa0: /* aftertouch of one key */
        for (int k = 0; k < NKEY; k++)
            if (key[k].held && key[k].note == data[0] && key[k].ch == ch) {
                key[k].poly = data[1] / 127.f;
                express(k);
            }
        break;
    case 0xd0: /* aftertouch of the channel: its keys without their own */
        press[ch] = data[0] / 127.f;
        express_ch(ch);
        break;
    case 0xe0:
        pb[ch] = (((data[1] << 7) | data[0]) - 8192) / 8192.f * (float)midi_cfg.bend;
        express_ch(ch);
        break;
    }
}

static void feed(const uint8_t *b, int n, uint64_t t)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xf8) /* real time: anywhere, even inside another message (the clock's) */
            continue;
        if (c >= 0xf0) { /* SysEx, system common: their data isn't a channel message's */
            status = 0;
            skip = c != 0xf7;
            continue;
        }
        if (c & 0x80) {
            skip = 0;
            status = c;
            ndata = 0;
            continue;
        }
        if (skip || !status)
            continue;
        data[ndata++] = c;
        if (ndata >= data_len(status)) {
            message(t);
            ndata = 0; /* running status: the same kind again */
        }
    }
}

void midi_feed(const uint8_t *b, int n, uint64_t t)
{
    if (!inited)
        init_clock(0);
    apply();
    mcio_feed(b, n, t);
    ui_lock();
    feed(b, n, t);
    ui_unlock();
}

/* a port's bytes, on brickio's reader thread (its clock hook has had the clock already) */
static void on_raw(int dev, const uint8_t *b, int n, uint64_t t_ns, void *ud)
{
    (void)dev, (void)ud;
    ui_lock();
    feed(b, n, t_ns / 1000);
    ui_unlock();
}

void midi_reset(void)
{
    for (int k = 0; k < NKEY; k++) {
        memset(&key[k], 0, sizeof key[k]);
        key[k].note = -1;
        engine_express_bend(NS + k, 0.f);
        engine_key_vibrato(NS + k, 0.f);
    }
    for (int c = 0; c < 16; c++)
        pb[c] = 0.f, sustain[c] = 0, press[c] = -1.f, modw[c] = 0.f;
    if (!inited)
        init_clock(0);
    else
        mclock_reset();
    status = 0, ndata = 0, skip = 0;
}

/* ------------------------------------------------------- clock out, link */

int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing)
{
    return linkpos_encode(out, max, heard, bpm, playing);
}

int midi_clock_step(uint64_t t, uint8_t *out, int max)
{
    if (!inited)
        init_clock(0);
    apply();
    return mclock_step(t, out, max);
}

int midi_link_step(uint64_t t, uint8_t *out, int max) { return mclock_link_step(t, out, max); }

/* ------------------------------------------------------------- running */

static int running, link_on;

static int env_on(const char *name)
{
    const char *v = getenv(name);
    return !(v && *v == '0');
}

void midi_io_start(void)
{
    const char *v = getenv("JD_MIDI_NOTES");
    int none = 1; /* as before the MIDI page: CHANNELS gives voices 1-5 channels 1-5, unless set there */
    for (int s = 0; s < NS; s++)
        none &= !midi_cfg.vch[s];
    if (v && !strcasecmp(v, "channels") && none)
        for (int s = 0; s < NS; s++)
            midi_cfg.vch[s] = s + 1;
    clock_out = env_on("JD_MIDI_CLOCK");
    follow = env_on("JD_MIDI_FOLLOW");
    relay = env_on("JD_MIDI_RELAY");
    init_clock(1); /* the reader's clock and SysEx hooks, the links */
    midi_reset();
    apply();
    /* the readers at real-time priority (below the sound, above the screen): each byte's time is taken as it arrives */
    midi_set_priority(40);
    midi_set_raw(on_raw, NULL);
    midi_start(NULL, NULL);
    /* the bottom port as a clock link: a Brick with a hub can lead this one (JD_LINK=0: left alone) */
    link_follow_priority(40);
    link_on = link_follow_start("JD", mcio_link_bytes, NULL, notice_line);
    mcio_start();
    running = 1;
}

void midi_io_stop(void)
{
    if (!running)
        return;
    running = 0;
    mcio_stop(); /* STOP out if the clock was going (and START OUT is on) */
    if (link_on)
        link_follow_stop(); /* the bottom port given back */
    midi_set_raw(NULL, NULL);
    midi_stop();
}

/* main thread, each frame: the settings into the clock, and MIDI ports coming and going */
void midi_tick(void)
{
    static int ports_was;
    apply();
    if (!running)
        return;
    int n = midi_count();
    if (n > ports_was)
        midi_notice("MIDI: %.40s", n == 1 ? midi_name() : "ANOTHER PORT");
    else if (n < ports_was)
        midi_notice("MIDI: %.40s", "A PORT WENT");
    ports_was = n;
}
