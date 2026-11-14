/* MIDI over USB (see midi.h). Everything here runs in the MIDI thread
 * except midi_take_notice; changes to the sound go through ui_lock, as the
 * buttons' do. */
#define _GNU_SOURCE
#include "midi.h"
#include "engine.h"
#include "link.h"
#include "aclock.h"
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

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------------ settings */

static int clock_out = 1, follow = 1;
static volatile float latency_s = 0.03f;

void midi_set_latency(float seconds) { latency_s = seconds < 0.f ? 0.f : seconds; }

/* the MIDI page's MIDI CLOCK switches over launch.sh's JD_MIDI_CLOCK / JD_MIDI_FOLLOW */
static int can_follow(void) { return follow && (song.gp[GP_MIDI] == MIDI_AUTO || song.gp[GP_MIDI] == MIDI_FOLLOW); }
static int can_send(void) { return clock_out && (song.gp[GP_MIDI] == MIDI_AUTO || song.gp[GP_MIDI] == MIDI_SEND); }
/* START, STOP, CONTINUE and song position too, unless set to the clock only */
static int can_transport(void) { return can_follow() && !song.gp[GP_MIDISTART]; }
/* START and STOP out with the clock, unless START OUT is off */
static int send_transport(void) { return midi_cfg.start_out; }

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

/* ------------------------------------------------------------ clock in */

static uint64_t tick_us, ext_seen_us;
static double tick_avg_us;
static double in_pos;     /* where the clock has the drone, in its 16ths: since START (or the song position) */
static volatile int following; /* an outside clock leads */
static int ext_running;
static int out_running; /* the clock going out */
static uint64_t precise_us; /* the last position from a leading Brick: its tempo and phase win over the ticks' */
static double pll_i;

int midi_following(void) { return following; }

/* where the sound is now: the audio clock, or the engine less the time to the speaker */
static double heard_now(uint64_t t)
{
    double h;
    return aclock_position(t, &h) ? h : position() - latency_s * engine_bpm() / 15.0;
}

/* the last ticks' times, for the tempo: a straight line through four beats
 * of them, which averages out the jitter of USB and the thread waking */
#define TK 96
static uint64_t tk_t[TK];
static int tk_n, tk_w;

static void clock_tick(uint64_t t)
{
    if (tick_us && (t <= tick_us || t - tick_us >= 250000))
        tk_n = 0; /* a gap: the fit starts afresh */
    tk_t[tk_w] = t;
    tk_w = (tk_w + 1) % TK;
    if (tk_n < TK)
        tk_n++;
    if (tk_n >= 2) {
        uint64_t t0 = tk_t[(tk_w - tk_n + TK) % TK];
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, n = tk_n;
        for (int i = 0; i < tk_n; i++) {
            double y = (double)(tk_t[(tk_w - tk_n + i + TK) % TK] - t0);
            sx += i, sy += y, sxx += (double)i * i, sxy += i * y;
        }
        double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        if (slope > 0.0)
            tick_avg_us = slope;
    }
    tick_us = t;
    ext_seen_us = t;
    if (!can_follow() || tick_avg_us <= 0.0)
        return;
    following = 1;
    if (precise_us && t - precise_us < 1000000) { /* a leading Brick's positions hold the tempo and phase */
        in_pos += in_ratio() / 6.0;
        return;
    }
    /* the tempo: set when it has really changed (the phase lock below takes
     * up what is left), quickly while the fit is young */
    float bpm = (float)(60e6 / (tick_avg_us * 24.0) * in_ratio()); /* IN DIV: 1/2 half time */
    bpm = bpm < 40.f ? 40.f : bpm > 250.f ? 250.f : bpm;
    if (fabsf(bpm - engine_bpm()) > (tk_n < 24 ? 0.05f : 0.15f))
        engine_set_bpm(roundf(bpm * 100.f) / 100.f);
    if (!ext_running || !live.playing)
        return;
    in_pos += in_ratio() / 6.0;
    /* the phase: what is heard (the audio clock) should be where the clock
     * is; trimmed gently, jumped only when far off, and a little integral
     * takes up the tempo's last difference */
    double err = in_pos - heard_now(t);
    if (fabs(err) > 2.0) {
        engine_set_position(position() + err);
        engine_set_rate(1.0);
        pll_i = 0.0;
    }
    else {
        pll_i = fmax(-0.004, fmin(0.004, pll_i + err * 0.002));
        engine_set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.05 + pll_i)));
    }
}

static void realtime(uint8_t b, uint64_t t)
{
    switch (b) {
    case 0xf8: clock_tick(t); break;
    case 0xfa: /* START: from the top */
        if (!can_transport())
            break;
        in_pos = 0.0;
        ext_running = 1;
        engine_set_position(latency_s * engine_bpm() / 15.0);
        engine_set_playing(1);
        break;
    case 0xfb: /* CONTINUE */
        if (!can_transport())
            break;
        ext_running = 1;
        engine_set_playing(1);
        break;
    case 0xfc: /* STOP */
        if (!can_transport())
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
static uint8_t sx[16]; /* a SysEx being read: only the Brick link's position is wanted */
static int nsx;

/* A leading Brick's position over the USB clock link (link.h), about 50 times
 * a second: F0 7D 'T' 'L', where its sound is (the 16ths heard at the moment
 * it was sent, x 65536, five 7-bit bytes, low first), its exact tempo (BPM x
 * 1000, three bytes) and whether it plays, F7. The same in Columbia Street,
 * HI5 and RadioScan, so any of them can lead this one. 7D is MIDI's id for
 * non-commercial use, which other gear passes over. The follower turns its
 * own heard position (the audio clock, aclock) toward it: a jump when more
 * than half a 16th off, else a slight change of speed, so the two sound
 * together within a fraction of a millisecond. Set to the clock only, it
 * lines up the bar but keeps its own count, and doesn't start or stop with
 * the leader. */
#define LINK_TRANSIT_S 0.0003 /* a bulk transfer through a hub and the follower's thread waking */
int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing)
{
    if (max < 14)
        return 0;
    uint64_t p = heard > 0.0 ? (uint64_t)llround(heard * 65536.0) : 0;
    uint32_t b = (uint32_t)lround(bpm * 1000.0);
    int n = 0;
    out[n++] = 0xf0, out[n++] = 0x7d, out[n++] = 'T', out[n++] = 'L';
    for (int k = 0; k < 5; k++)
        out[n++] = (uint8_t)(p >> (7 * k) & 127);
    for (int k = 0; k < 3; k++)
        out[n++] = (uint8_t)(b >> (7 * k) & 127);
    out[n++] = playing ? 1 : 0;
    out[n++] = 0xf7;
    return n;
}

static void link_position(uint64_t t, double lead, double bpm, int lead_playing)
{
    if (!can_follow())
        return;
    ext_seen_us = precise_us = t;
    following = 1;
    if (bpm >= 40.0 && bpm <= 250.0 && fabs(bpm - engine_bpm()) > 1e-4)
        engine_set_bpm((float)bpm);
    if (!live.playing) {
        if (lead_playing && can_transport()) { /* joined mid-song: in at the leader's place */
            engine_set_playing(1);
            engine_set_position(lead + (LINK_TRANSIT_S + latency_s) * bpm / 15.0);
            ext_running = 1;
            pll_i = 0.0;
        }
        return;
    }
    if (!lead_playing) { /* the leader stopped (a leader passing on another's clock sends no STOP) */
        if (can_transport()) {
            ext_running = 0;
            engine_set_rate(1.0);
            engine_set_playing(0);
        }
        return;
    }
    double err = lead + LINK_TRANSIT_S * bpm / 15.0 - heard_now(t);
    if (!can_transport()) /* the bar only: the nearest way round */
        err -= 16.0 * floor((err + 8.0) / 16.0);
    if (fabs(err) > 0.5) {
        engine_set_position(position() + err);
        engine_set_rate(1.0);
        pll_i = 0.0;
        return;
    }
    pll_i = fmax(-0.003, fmin(0.003, pll_i + err * 0.02));
    engine_set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.4 + pll_i)));
}

static void sysex_end(uint64_t t)
{
    if (nsx != 12 || sx[0] != 0x7d || sx[1] != 'T' || sx[2] != 'L')
        return;
    uint64_t p = 0;
    uint32_t b = 0;
    for (int k = 0; k < 5; k++)
        p |= (uint64_t)sx[3 + k] << (7 * k);
    for (int k = 0; k < 3; k++)
        b |= (uint32_t)sx[8 + k] << (7 * k);
    link_position(t, p / 65536.0, b / 1000.0, sx[11] & 1);
}

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
    case 0xf0:
        if (status == 0xf2 && can_transport()) { /* song position, in 16ths */
            in_pos = ((data[1] << 7) | data[0]) * in_ratio();
            engine_set_position(in_pos + latency_s * engine_bpm() / 15.0);
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
            nsx = 0;
            continue;
        }
        if (c == 0xf7) {
            if (in_sysex)
                sysex_end(t);
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
        if (in_sysex) {
            if (nsx < (int)sizeof sx)
                sx[nsx++] = c;
            else
                nsx = (int)sizeof sx + 1; /* too long: not ours */
            continue;
        }
        if (!status)
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
    for (int k = 0; k < NKEY; k++) {
        memset(&key[k], 0, sizeof key[k]);
        key[k].note = -1;
        engine_express_bend(NS + k, 0.f);
        engine_key_vibrato(NS + k, 0.f);
    }
    for (int c = 0; c < 16; c++)
        pb[c] = 0.f, sustain[c] = 0, press[c] = -1.f, modw[c] = 0.f;
    tick_us = ext_seen_us = 0;
    tick_avg_us = 0.0;
    in_pos = 0.0;
    following = ext_running = 0;
    status = 0, ndata = 0, in_sysex = 0, nsx = 0;
    precise_us = 0, pll_i = 0.0;
    tk_n = tk_w = 0;
    out_running = 0;
}

/* ------------------------------------------------------------ clock out */

static double out_est, out_r = 1.0; /* out_r: the OUT DIV the ticks were counted at */
static int64_t out_sent;
static uint64_t out_t;

int midi_clock_step(uint64_t t, uint8_t *out, int max)
{
    int n = 0;
    int play = live.playing && can_send() && !following;
    if (!play) {
        if (out_running && send_transport() && n < max)
            out[n++] = 0xfc;
        out_running = 0;
        return n;
    }
    double bpm = engine_bpm(), r = out_ratio(); /* OUT DIV: x2 sends double time */
    /* where the sound is, through whichever output plays it (a USB audio
     * interface's own buffering included): the audio clock */
    double heard = heard_now(t);
    if (!out_running) { /* START now: the next tick is the first */
        out_running = 1;
        out_est = heard;
        out_r = r;
        out_sent = (int64_t)floor(heard * 6.0 * r);
        out_t = t;
        if (send_transport() && n < max)
            out[n++] = 0xfa;
        return n;
    }
    /* the engine's position moves in blocks: a smooth estimate between them,
     * held to it */
    out_est += (double)(t - out_t) * 1e-6 * bpm / 15.0;
    out_t = t;
    double d = heard - out_est;
    out_est += fabs(d) > 1.0 ? d : d * 0.02;
    int64_t want = (int64_t)floor(out_est * 6.0 * r);
    if (r != out_r) { /* OUT DIV changed: count on from here at the new rate */
        out_r = r;
        out_sent = want;
    }
    if (want - out_sent > (r > 1.0 ? 4 * r : 4)) /* a jump (a new position): no burst of ticks */
        out_sent = want - 1;
    while (out_sent < want && n < max) {
        out[n++] = 0xf8;
        out_sent++;
    }
    if (want < out_sent)
        out_sent = want; /* moved back: wait for it */
    return n;
}

/* the position for the Bricks following over the link, every 20 ms while the
 * clock goes out, or while this one follows another clock (passing it on):
 * the bytes to send now, or 0 */
static uint64_t link_sent_us;
int midi_link_step(uint64_t t, uint8_t *out, int max)
{
    if (!out_running && !following) {
        link_sent_us = 0;
        return 0;
    }
    if (link_sent_us && t - link_sent_us < 20000)
        return 0;
    link_sent_us = t;
    return midi_link_pos(out, max, heard_now(t), engine_bpm(), live.playing);
}

/* --------------------------------------------------------------- ports */

#define MAXPORT 8
/* a raw MIDI port, or a Brick following this one over the USB clock link
 * (link.h): its usbfs device, written with bulk transfers to its endpoint */
typedef struct {
    int fd;
    int link_ep, dead; /* a link: its bulk OUT endpoint (0 for a MIDI port); it went */
    char path[300], name[32];
} Port;
static Port ports[MAXPORT];
static int nports;
int midi_ports(void) { return nports; }
int midi_links(void)
{
    int n = 0;
    for (int i = 0; i < nports; i++)
        n += ports[i].link_ep != 0;
    return n;
}

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

static void drop(int i);

/* Bricks following this one over the USB clock link: new ones opened, gone ones dropped */
static void scan_links(void)
{
    LinkDev ld[MAXPORT];
    int n = link_find("/sys/bus/usb/devices", ld, MAXPORT);
    for (int i = nports - 1; i >= 0; i--) {
        if (!ports[i].link_ep)
            continue;
        int still = 0;
        for (int k = 0; k < n && !ports[i].dead; k++)
            still |= !strcmp(ld[k].path, ports[i].path);
        if (!still)
            drop(i);
    }
    for (int k = 0; k < n && nports < MAXPORT; k++) {
        int known = 0;
        for (int i = 0; i < nports; i++)
            known |= ports[i].link_ep && !strcmp(ports[i].path, ld[k].path);
        if (known)
            continue;
        char dev[64];
        snprintf(dev, sizeof dev, "/dev/bus/usb/%03d/%03d", ld[k].bus, ld[k].dev);
        int fd = open(dev, O_RDWR | O_CLOEXEC);
        unsigned iface = 0;
        if (fd < 0 || ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) {
            if (fd >= 0)
                close(fd);
            continue;
        }
        Port *p = &ports[nports++];
        memset(p, 0, sizeof *p);
        p->fd = fd;
        p->link_ep = ld[k].ep;
        snprintf(p->path, sizeof p->path, "%s", ld[k].path);
        snprintf(p->name, sizeof p->name, "BRICK LINK");
        midi_notice("LINK: %.40s", "A BRICK FOLLOWS THIS ONE");
    }
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
        memset(p, 0, sizeof *p);
        p->fd = fd;
        snprintf(p->path, sizeof p->path, "%s", path);
        port_name(card, p->name, sizeof p->name);
        midi_notice("MIDI: %.40s", p->name);
    }
    closedir(d);
    scan_links();
}

static void drop(int i)
{
    if (ports[i].link_ep)
        midi_notice("LINK: %.40s", "A FOLLOWING BRICK WENT");
    else
        midi_notice("MIDI: %.34s GONE", ports[i].name);
    close(ports[i].fd);
    ports[i] = ports[--nports];
}

/* to one Brick following over the link: a bulk transfer, given up after 20 ms */
static void send_link(Port *p, const uint8_t *b, int n)
{
    if (p->dead)
        return;
    struct usbdevfs_bulktransfer bt = {.ep = (unsigned)p->link_ep, .len = (unsigned)n, .timeout = 20, .data = (void *)b};
    if (ioctl(p->fd, USBDEVFS_BULK, &bt) < 0 && (errno == ENODEV || errno == ESHUTDOWN))
        p->dead = 1; /* dropped at the next scan */
}

static void send_all(const uint8_t *b, int n)
{
    for (int i = 0; i < nports; i++) {
        if (ports[i].link_ep) {
            send_link(&ports[i], b, n);
            continue;
        }
        if (write(ports[i].fd, b, (size_t)n) < 0) {
            /* a full or vanished port: a port that went away is dropped when read */
        }
    }
}

/* to the Bricks following over the link only */
static void send_links(const uint8_t *b, int n)
{
    for (int i = 0; i < nports; i++)
        if (ports[i].link_ep)
            send_link(&ports[i], b, n);
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
    uint64_t next_scan = 0;
    while (running) {
        uint64_t t = now_us();
        if (t >= next_scan) {
            scan();
            next_scan = t + 1000000;
        }
        struct pollfd pf[MAXPORT];
        for (int i = 0; i < nports; i++)
            pf[i].fd = ports[i].link_ep ? -1 : ports[i].fd, pf[i].events = POLLIN, pf[i].revents = 0; /* links only send */
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
        /* the outside clock has stopped, or MIDI is set not to follow it */
        int stale = following && (t - ext_seen_us > 1000000 || !can_follow());
        if (stale) { /* the outside clock has stopped */
            ui_lock();
            following = ext_running = 0;
            engine_set_rate(1.0);
            ui_unlock();
        }
        uint8_t out[16];
        int n = midi_clock_step(t, out, sizeof out);
        if (n)
            send_all(out, n);
        uint8_t pos[16];
        if (midi_links() && (n = midi_link_step(t, pos, sizeof pos)) > 0)
            send_links(pos, n);
    }
    uint8_t stop = 0xfc;
    if (out_running && send_transport())
        send_all(&stop, 1);
    for (int i = 0; i < nports; i++)
        close(ports[i].fd);
    nports = 0;
    return NULL;
}

void midi_start(int core)
{
    const char *v = getenv("JD_MIDI_NOTES");
    int none = 1; /* as before the MIDI page: CHANNELS gives voices 1-5 channels 1-5, unless set there */
    for (int s = 0; s < NS; s++)
        none &= !midi_cfg.vch[s];
    if (v && !strcasecmp(v, "channels") && none)
        for (int s = 0; s < NS; s++)
            midi_cfg.vch[s] = s + 1;
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
