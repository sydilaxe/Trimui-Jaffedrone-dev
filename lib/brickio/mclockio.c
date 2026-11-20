/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The musical clock, wired: see mclockio.h. */
#define _GNU_SOURCE
#include "mclockio.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "link.h"
#include "linkpos.h"
#include "midi.h"

static McioConfig cfg;
static atomic_int src_dev = -1; /* the MIDI port the clock comes in on (relayed clock skips it) */

static void lk(void) { if (cfg.lock) cfg.lock(); }
static void ul(void) { if (cfg.unlock) cfg.unlock(); }

static uint64_t now_us(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000ull + (uint64_t)t.tv_nsec / 1000;
}

/* ------------------------------------------------------------------ in */

static void realtime(uint8_t b, uint64_t t)
{
    if (b < 0xf8 || b > 0xfc || b == 0xf9) return;
    lk();
    mclock_realtime(b, t);
    ul();
}

static void songpos(int sixteenths)
{
    lk();
    mclock_song_position(sixteenths);
    ul();
}

static void sysex(const uint8_t *m, int n, uint64_t t)
{
    double heard, bpm;
    int playing;
    if (!linkpos_decode(m, n, &heard, &bpm, &playing)) return;
    lk();
    mclock_link_position(t, heard, bpm, playing);
    ul();
}

static void on_clock(int dev, uint8_t b, int pos, uint64_t t_ns, void *ud)
{
    (void)ud;
    if (b == 0xf2) { songpos(pos); return; }
    atomic_store(&src_dev, dev); /* the port the clock (or its START) comes in on: a relay leaves it out */
    realtime(b, t_ns / 1000);
}

static void on_sysex(int dev, const uint8_t *m, int n, uint64_t t_ns, void *ud)
{
    (void)dev, (void)ud;
    sysex(m, n, t_ns / 1000);
}

/* the parser for mcio_feed: real-time bytes anywhere, F2 and its two bytes, a SysEx up to 16 bytes */
static pthread_mutex_t feed_mx = PTHREAD_MUTEX_INITIALIZER;
static uint8_t fsx[16], fcd[2];
static int fsxn = -1, fcommon, fcn; /* fsxn: -1 not in a SysEx, 17 one too long; fcommon: 2 song position, -1 F1 / F3 */

static void feed(const uint8_t *b, int n, uint64_t t)
{
    pthread_mutex_lock(&feed_mx);
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xf8) { realtime(c, t); continue; }
        if (c == 0xf0) { fsxn = 0, fcommon = 0; continue; }
        if (c == 0xf7) {
            if (fsxn >= 0 && fsxn <= (int)sizeof fsx) sysex(fsx, fsxn, t);
            fsxn = -1;
            continue;
        }
        if (c & 0x80) { /* any other status: F2 waits for two bytes, F1 / F3 one (passed by) */
            fsxn = -1, fcn = 0;
            fcommon = c == 0xf2 ? 2 : c == 0xf1 || c == 0xf3 ? -1 : 0;
            continue;
        }
        if (fsxn >= 0) { if (fsxn < (int)sizeof fsx) fsx[fsxn++] = c; else fsxn = (int)sizeof fsx + 1; continue; }
        if (fcommon == 2) {
            fcd[fcn++] = c;
            if (fcn == 2) songpos(fcd[0] | fcd[1] << 7), fcommon = 0;
        }
        else if (fcommon == -1) fcommon = 0; /* F1 / F3's one byte */
    }
    pthread_mutex_unlock(&feed_mx);
}

void mcio_feed(const uint8_t *b, int n, uint64_t t_us) { feed(b, n, t_us); }
void mcio_link_bytes(const uint8_t *b, int n, uint64_t t_us, void *ud) { (void)ud; feed(b, n, t_us); }

/* ------------------------------------------------------------------ the links */

#define MAXLINK 8
typedef struct {
    int fd, ep;
    char path[300];
} Lnk;
static Lnk lnk[MAXLINK];
static atomic_int nlnk;

int mcio_links(void) { return atomic_load(&nlnk); }

static void say(const char *m) { if (cfg.notice) cfg.notice(m); }

static void links_scan(void)
{
    LinkDev ld[MAXLINK];
    int n = link_find(cfg.sysfs ? cfg.sysfs : "/sys/bus/usb/devices", ld, MAXLINK), have = atomic_load(&nlnk);
    for (int i = have - 1; i >= 0; i--) {
        int still = 0;
        for (int k = 0; k < n; k++) still |= !strcmp(ld[k].path, lnk[i].path);
        if (still && lnk[i].fd >= 0) continue;
        if (lnk[i].fd >= 0) close(lnk[i].fd);
        lnk[i] = lnk[--have];
        say("LINK: A FOLLOWING BRICK WENT");
    }
    for (int k = 0; k < n && have < MAXLINK; k++) {
        int known = 0;
        for (int i = 0; i < have; i++) known |= !strcmp(lnk[i].path, ld[k].path);
        if (known) continue;
        int fd = link_open(&ld[k]);
        if (fd < 0) continue;
        lnk[have].fd = fd, lnk[have].ep = ld[k].ep;
        snprintf(lnk[have].path, sizeof lnk[have].path, "%s", ld[k].path);
        have++;
        say("LINK: A BRICK FOLLOWS THIS ONE");
    }
    atomic_store(&nlnk, have);
}

static void links_send(const uint8_t *b, int n)
{
    int have = atomic_load(&nlnk);
    for (int i = 0; i < have; i++)
        if (lnk[i].fd >= 0 && link_send(lnk[i].fd, lnk[i].ep, b, n) < 0) close(lnk[i].fd), lnk[i].fd = -1; /* gone: dropped at the next scan */
}

/* ------------------------------------------------------------------ out */

static pthread_t thr;
static atomic_int running;

static void *loop(void *arg)
{
    (void)arg;
    struct sched_param sp = {.sched_priority = cfg.prio > 0 ? cfg.prio : 45};
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp) != 0)
        fprintf(stderr, "mclockio: the clock thread at normal priority (real time refused)\n");
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    uint64_t scan_at = 0;
    while (atomic_load(&running)) {
        uint64_t t = now_us();
        if (cfg.links && t >= scan_at) links_scan(), scan_at = t + 1000000;
        if (mclock_following()) { /* let go of a clock that has stopped */
            lk();
            mclock_watch(t);
            ul();
        }
        uint8_t out[16];
        int n = mclock_step(t, out, sizeof out);
        if (n) {
            if (cfg.midi) midi_send_except(mclock_following() ? atomic_load(&src_dev) : -1, out, n);
            if (cfg.links) links_send(out, n);
        }
        if (cfg.links && atomic_load(&nlnk) && (n = mclock_link_step(t, out, sizeof out)) > 0) links_send(out, n);
        /* every millisecond, on an absolute timetable; caught up rather than bunched after a long hold-up */
        next.tv_nsec += 1000000;
        if (next.tv_nsec >= 1000000000) next.tv_sec++, next.tv_nsec -= 1000000000;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if ((now.tv_sec - next.tv_sec) * 1000000000L + (now.tv_nsec - next.tv_nsec) > 5000000L) next = now;
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR) {}
    }
    if (mclock_sending()) {
        const uint8_t stop = 0xfc;
        if (cfg.midi) midi_send(&stop, 1);
        if (cfg.links) links_send(&stop, 1);
    }
    for (int i = 0; i < atomic_load(&nlnk); i++)
        if (lnk[i].fd >= 0) close(lnk[i].fd);
    atomic_store(&nlnk, 0);
    return NULL;
}

void mcio_init(const McioConfig *c)
{
    cfg = *c;
    mclock_init(c->host);
    atomic_store(&src_dev, -1);
    pthread_mutex_lock(&feed_mx);
    fsxn = -1, fcommon = fcn = 0;
    pthread_mutex_unlock(&feed_mx);
    if (c->midi) {
        midi_set_clock(on_clock, NULL);
        midi_set_sysex(on_sysex, NULL);
    }
}

int mcio_start(void)
{
    if (atomic_exchange(&running, 1)) return 0;
    if (pthread_create(&thr, NULL, loop, NULL) != 0) {
        atomic_store(&running, 0);
        return -1;
    }
    return 0;
}

void mcio_stop(void)
{
    if (!atomic_exchange(&running, 0)) return;
    pthread_join(thr, NULL);
    if (cfg.midi) midi_set_clock(NULL, NULL), midi_set_sysex(NULL, NULL);
}
