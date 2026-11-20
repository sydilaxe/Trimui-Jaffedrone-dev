/* SPDX-License-Identifier: GPL-3.0-or-later */
/* USB MIDI: see midi.h. */
#define _GNU_SOURCE
#include "midi.h"

#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define MAXDEV 8
#define QSIZE 1024 /* a power of two */

#define SXMAX 64 /* the longest SysEx kept (the link position is 14 bytes; longer ones are passed by) */

typedef struct {
    int fd, out; /* out: opened for writing too */
    char path[256], name[48];
    uint8_t status, d[2];
    int nd, sysex;
    uint8_t sx[SXMAX];
    uint8_t common, cd[2]; /* a system common message being read (F1 F2 F3): its data */
    int cn;
    int sxn; /* -1: too long, being skipped */
} Dev;

static Dev dev[MAXDEV];
static int ndev;
/* recursive: the raw tap and the SysEx hook may send from inside the reader */
static pthread_mutex_t lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP;
static pthread_t thr;
static atomic_int running;
static char pat[256];
static void (*wake_fn)(void);
static MidiEv q[QSIZE];
static atomic_uint qw, qr;
static void (*raw_fn)(int, const uint8_t *, int, uint64_t, void *);
static void *raw_ud;



static uint64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec;
}

static void card_name(const char *path, char *out, int n)
{
    int card = -1, d = 0;
    snprintf(out, (size_t)n, "MIDI");
    if (sscanf(path, "/dev/snd/midiC%dD%d", &card, &d) < 1) return;
    char p[64];
    snprintf(p, sizeof p, "/proc/asound/card%d/id", card);
    FILE *f = fopen(p, "r");
    if (!f) return;
    if (fgets(out, n, f)) out[strcspn(out, "\r\n")] = 0;
    fclose(f);
}

/* (the lock held) */
static void scan(void)
{
    glob_t g;
    if (glob(*pat ? pat : "/dev/snd/midiC*D*", 0, NULL, &g) != 0) return;
    for (size_t i = 0; i < g.gl_pathc && ndev < MAXDEV; i++) {
        int have = 0;
        for (int k = 0; k < ndev; k++)
            if (!strcmp(dev[k].path, g.gl_pathv[i])) have = 1;
        if (have) continue;
        int out = 1, fd = open(g.gl_pathv[i], O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) out = 0, fd = open(g.gl_pathv[i], O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        Dev *d = &dev[ndev++];
        memset(d, 0, sizeof *d);
        d->fd = fd, d->out = out;
        snprintf(d->path, sizeof d->path, "%s", g.gl_pathv[i]);
        card_name(d->path, d->name, sizeof d->name);
        fprintf(stderr, "midi: opened %s (%s)%s\n", d->path, d->name, out ? ", in and out" : "");
    }
    globfree(&g);
}

/* (the lock held) */
static void drop(int k)
{
    fprintf(stderr, "midi: %s gone\n", dev[k].path);
    close(dev[k].fd);
    dev[k] = dev[--ndev];
}

/* one byte through the parser; 1 when a message is complete */
static int parse(Dev *d, uint8_t b, MidiEv *e)
{
    if (b >= 0xf8) { /* real-time: between any bytes, running status kept */
        e->ch = 0, e->note = 0, e->num = 0, e->val = 0;
        switch (b) {
        case 0xf8: e->type = MIDI_CLOCK; return 1;
        case 0xfa: e->type = MIDI_START; return 1;
        case 0xfb: e->type = MIDI_CONTINUE; return 1;
        case 0xfc: e->type = MIDI_STOP; return 1;
        default: return 0;
        }
    }
    if (b == 0xf0) { d->sysex = 1, d->sx[0] = 0xf0, d->sxn = 1; return 0; }
    if (b == 0xf7 && d->sysex) { /* a SysEx complete: 2 */
        d->sysex = 0, d->status = 0;
        if (d->sxn < 0 || d->sxn >= SXMAX) return 0;
        d->sx[d->sxn++] = 0xf7;
        return 2;
    }
    /* system common: F2 song position (two data bytes; 3 when complete), F1 and F3 (one, passed by), the
     * others none. They cancel running status (MIDI 1.0), so their data never reads as a channel message's */
    if (b >= 0xf0) { d->sysex = 0; d->status = 0; d->common = b == 0xf1 || b == 0xf2 || b == 0xf3 ? b : 0; d->cn = 0; return 0; }
    if (b & 0x80) { d->sysex = 0; d->common = 0; d->status = b; d->nd = 0; return 0; }
    if (d->common) {
        d->cd[d->cn++] = b;
        if (d->cn < (d->common == 0xf2 ? 2 : 1)) return 0;
        int songpos = d->common == 0xf2;
        d->common = 0;
        return songpos ? 3 : 0;
    }
    if (d->sysex) {
        if (d->sxn >= 0 && d->sxn < SXMAX - 1) d->sx[d->sxn++] = b;
        else d->sxn = -1;
        return 0;
    }
    if (!d->status) return 0;
    int hi = d->status & 0xf0, need = (hi == 0xc0 || hi == 0xd0) ? 1 : 2;
    d->d[d->nd++] = b;
    if (d->nd < need) return 0;
    d->nd = 0;
    e->ch = d->status & 15, e->note = 0, e->num = d->d[0], e->val = need > 1 ? d->d[1] : 0;
    switch (hi) {
    case 0xb0: e->type = MIDI_CC; return 1;
    case 0x90: e->type = MIDI_NOTE, e->note = 1; return 1;
    case 0x80: e->type = MIDI_NOTE, e->note = 1, e->val = 0; return 1;
    case 0xc0: e->type = MIDI_PROGRAM; return 1;
    case 0xe0: e->type = MIDI_BEND; return 1;
    case 0xd0: case 0xa0: e->type = MIDI_TOUCH; if (hi == 0xd0) e->val = e->num, e->num = 0; return 1;
    }
    return 0;
}

/* (the lock held) */
static void (*sysex_fn)(int, const uint8_t *, int, uint64_t, void *);
static void *sysex_ud;
static void (*clock_fn)(int, uint8_t, int, uint64_t, void *);
static void *clock_ud;

void midi_set_clock(void (*fn)(int dev, uint8_t b, int songpos, uint64_t t_ns, void *ud), void *ud)
{
    pthread_mutex_lock(&lock);
    clock_fn = fn, clock_ud = ud;
    pthread_mutex_unlock(&lock);
}

void midi_set_sysex(void (*fn)(int dev, const uint8_t *msg, int n, uint64_t t_ns, void *ud), void *ud)
{
    pthread_mutex_lock(&lock);
    sysex_fn = fn, sysex_ud = ud;
    pthread_mutex_unlock(&lock);
}

void midi_set_raw(void (*fn)(int dev, const uint8_t *bytes, int n, uint64_t t_ns, void *ud), void *ud)
{
    pthread_mutex_lock(&lock);
    raw_fn = fn, raw_ud = ud;
    pthread_mutex_unlock(&lock);
}

static atomic_int prio_want;
void midi_set_priority(int prio) { atomic_store(&prio_want, prio < 0 ? 0 : prio > 99 ? 99 : prio); }

static void *reader(void *arg)
{
    (void)arg;
    int prio = atomic_load(&prio_want);
    if (prio > 0) {
        struct sched_param sp = {.sched_priority = prio};
        int e = pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
        fprintf(stderr, "midi: reader %s\n", e ? "at normal priority (real time refused)" : "at real-time priority");
    }
    uint64_t next_scan = 0;
    while (atomic_load(&running)) {
        struct pollfd pf[MAXDEV];
        int n;
        pthread_mutex_lock(&lock);
        if (now_ns() >= next_scan) scan(), next_scan = now_ns() + 2000000000ull;
        n = ndev;
        for (int k = 0; k < n; k++) pf[k].fd = dev[k].fd, pf[k].events = POLLIN, pf[k].revents = 0;
        pthread_mutex_unlock(&lock);
        if (!n) { usleep(200000); continue; }
        if (poll(pf, (nfds_t)n, 200) <= 0) continue;
        int queued = 0;
        pthread_mutex_lock(&lock);
        for (int k = n - 1; k >= 0; k--) {
            if (k >= ndev || dev[k].fd != pf[k].fd || !pf[k].revents) continue;
            uint8_t buf[256];
            ssize_t got = read(dev[k].fd, buf, sizeof buf);
            if (got < 0 && errno != EAGAIN && errno != EINTR) { drop(k); continue; }
            if ((pf[k].revents & (POLLERR | POLLHUP | POLLNVAL)) && got <= 0) { drop(k); continue; }
            uint64_t t = now_ns();
            if (got > 0 && raw_fn) raw_fn(k, buf, (int)got, t, raw_ud);
            for (ssize_t i = 0; i < got && k < ndev; i++) {
                MidiEv e;
                int r = parse(&dev[k], buf[i], &e);
                if (r == 2) { if (sysex_fn) sysex_fn(k, dev[k].sx, dev[k].sxn, t, sysex_ud); continue; }
                if (r == 3) { if (clock_fn) clock_fn(k, 0xf2, dev[k].cd[0] | dev[k].cd[1] << 7, t, clock_ud); continue; }
                if (!r) continue;
                if (e.type >= MIDI_CLOCK && clock_fn) clock_fn(k, buf[i], -1, t, clock_ud);
                e.t_ns = t;
                unsigned w = atomic_load_explicit(&qw, memory_order_relaxed);
                if (w - atomic_load_explicit(&qr, memory_order_acquire) >= QSIZE) continue; /* full: dropped */
                q[w & (QSIZE - 1)] = e;
                atomic_store_explicit(&qw, w + 1, memory_order_release);
                queued = 1;
            }
        }
        pthread_mutex_unlock(&lock);
        if (queued && wake_fn) wake_fn();
    }
    return NULL;
}

int midi_start(const char *pattern, void (*wake)(void))
{
    if (atomic_load(&running)) return 0;
    snprintf(pat, sizeof pat, "%s", pattern ? pattern : "");
    wake_fn = wake;
    atomic_store(&running, 1);
    if (pthread_create(&thr, NULL, reader, NULL) != 0) { atomic_store(&running, 0); return -1; }
    return 0;
}

int midi_poll(MidiEv *out, int max)
{
    unsigned r = atomic_load_explicit(&qr, memory_order_relaxed), w = atomic_load_explicit(&qw, memory_order_acquire);
    int n = 0;
    while (r != w && n < max) out[n++] = q[r & (QSIZE - 1)], r++;
    atomic_store_explicit(&qr, r, memory_order_release);
    return n;
}

int midi_send(const uint8_t *bytes, int n)
{
    int took = 0;
    pthread_mutex_lock(&lock);
    for (int k = 0; k < ndev; k++)
        if (dev[k].out && write(dev[k].fd, bytes, (size_t)n) == n) took++;
    pthread_mutex_unlock(&lock);
    return took;
}

int midi_send_except(int skip, const uint8_t *bytes, int n)
{
    int took = 0;
    pthread_mutex_lock(&lock);
    for (int k = 0; k < ndev; k++)
        if (k != skip && dev[k].out && write(dev[k].fd, bytes, (size_t)n) == n) took++;
    pthread_mutex_unlock(&lock);
    return took;
}

int midi_count(void)
{
    pthread_mutex_lock(&lock);
    int n = ndev;
    pthread_mutex_unlock(&lock);
    return n;
}

const char *midi_name(void)
{
    static char name[48];
    pthread_mutex_lock(&lock);
    snprintf(name, sizeof name, "%s", ndev ? dev[0].name : "");
    pthread_mutex_unlock(&lock);
    return name;
}

/* ---------------------------------------------------------------- clock out
 * A thread of its own sends the 24-a-beat clock on an absolute timetable (each tick from the first,
 * never from the last: no drift), at a real-time priority below the audio threads'. Start (FA) as it
 * begins, stop (FC) as it ends. */
static pthread_t ck_thr;
static atomic_int ck_on;          /* the thread runs */
static _Atomic float ck_bpm;      /* 0: stopped */
static pthread_mutex_t ck_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ck_cond = PTHREAD_COND_INITIALIZER;

static void *clock_thread(void *arg)
{
    (void)arg;
    struct sched_param sp = {.sched_priority = 60};
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    int sending = 0;
    uint64_t next = 0;
    double bpm_was = 0.0;
    while (atomic_load(&ck_on)) {
        float bpm = atomic_load(&ck_bpm);
        if (bpm <= 0.f) {
            if (sending) { const uint8_t stop = 0xFC; midi_send(&stop, 1); sending = 0; }
            pthread_mutex_lock(&ck_lock);
            while (atomic_load(&ck_on) && atomic_load(&ck_bpm) <= 0.f) pthread_cond_wait(&ck_cond, &ck_lock);
            pthread_mutex_unlock(&ck_lock);
            continue;
        }
        if (!sending) { const uint8_t start = 0xFA; midi_send(&start, 1); sending = 1, next = now_ns(), bpm_was = bpm; }
        if (bpm != bpm_was) bpm_was = bpm; /* a new tempo from the next tick on */
        const uint8_t tick = 0xF8;
        midi_send(&tick, 1);
        next += (uint64_t)(60e9 / (bpm_was * 24.0));
        struct timespec ts = {(time_t)(next / 1000000000ull), (long)(next % 1000000000ull)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
        uint64_t now = now_ns();
        if (now > next + 100000000ull) next = now; /* held up badly (100 ms): carry on from now */
    }
    if (sending) { const uint8_t stop = 0xFC; midi_send(&stop, 1); }
    return NULL;
}

void midi_clock_out(float bpm)
{
    if (bpm > 0.f && (bpm < 20.f || bpm > 300.f)) return;
    atomic_store(&ck_bpm, bpm);
    if (bpm > 0.f && !atomic_load(&ck_on)) {
        atomic_store(&ck_on, 1);
        if (pthread_create(&ck_thr, NULL, clock_thread, NULL) != 0) { atomic_store(&ck_on, 0); return; }
    }
    pthread_mutex_lock(&ck_lock);
    pthread_cond_signal(&ck_cond);
    pthread_mutex_unlock(&ck_lock);
}

void midi_stop(void)
{
    if (atomic_exchange(&ck_on, 0)) {
        pthread_mutex_lock(&ck_lock);
        pthread_cond_signal(&ck_cond);
        pthread_mutex_unlock(&ck_lock);
        pthread_join(ck_thr, NULL);
    }
    if (atomic_exchange(&running, 0)) pthread_join(thr, NULL);
    pthread_mutex_lock(&lock);
    while (ndev) close(dev[ndev - 1].fd), ndev--;
    pthread_mutex_unlock(&lock);
}
