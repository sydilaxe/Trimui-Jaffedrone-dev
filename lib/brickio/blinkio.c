/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Brick link's data channel in use (see blinkio.h). */
#define _GNU_SOURCE
#include "blinkio.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usbdevice_fs.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <time.h>
#include <unistd.h>

#include "aring.h"
#include "link.h"

#define OUT_FRAMES 8192          /* each port's outgoing ring: a power of two, 170 ms at 48 kHz */
#define CHUNK 512                /* audio frames to a frame at most */
#define CAP (BLINK_MAX_TRANSFER - BLINK_HDR) /* a transfer's frames, with room for blink_pad */
#define HELLO_EVERY_US 500000
#define PEER_GONE_US 1500000
#define MIDI_Q 8192

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}
static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

typedef struct {
    float buf[OUT_FRAMES * 2];
    atomic_uint w, r;
    _Atomic double heard;
} OutRing;

typedef struct {
    uint8_t b[BLINK_MAX_TRANSFER];
    int n;
} Xfer;

struct BlSession {
    BlConfig c;
    BlTransport t;
    pthread_t tx_th, rx_th;
    atomic_int running, tx_done, rx_done, gone;
    sem_t wake;

    OutRing out[BL_PORTS];
    uint16_t tx_seq[BL_PORTS], midi_seq, misc_seq;
    atomic_ullong hello_due;
    /* the sending thread's own */
    Xfer x;
    uint8_t pay[BLINK_MAX_PAYLOAD], mq_copy[MIDI_Q];
    float tx_tmp[CHUNK * 2];

    /* MIDI waiting to go: records of port, length (2 bytes), bytes */
    pthread_mutex_t mq_lock;
    uint8_t mq[MIDI_Q];
    int mq_n;

    atomic_ullong ping_out; /* a PING to send (its time), 0 none */
    atomic_ullong pong_out; /* a PONG to send (the time it echoes), 0 none */

    /* the other side */
    pthread_mutex_t peer_lock;
    BlinkHello peer;
    atomic_int peer_up;
    atomic_ullong last_heard;

    /* audio in */
    ARing *_Atomic in[BL_PORTS];
    atomic_int in_block[BL_PORTS], in_rate[BL_PORTS];
    _Atomic double in_heard[BL_PORTS];
    int in_seen[BL_PORTS];
    uint16_t in_seq[BL_PORTS];
    BlinkRx rx;
    /* the receiving thread's own */
    uint8_t rx_buf[BLINK_MAX_TRANSFER];
    float rx_tmp[BLINK_MAX_PAYLOAD / 4];

    /* counters */
    atomic_uint tx_frames, tx_drops, rx_frames, rx_lost, transfers_out, transfers_in, send_fails;
    atomic_uint rtt_us;
};

static void on_signal(int sig) { (void)sig; }

/* ------------------------------------------------------------ the other side */

static void peer_check(BlSession *s)
{
    uint64_t t = atomic_load(&s->last_heard);
    if (atomic_load(&s->peer_up) && now_us() - t > PEER_GONE_US && atomic_exchange(&s->peer_up, 0)) {
        if (s->c.on_peer) {
            BlinkHello h;
            pthread_mutex_lock(&s->peer_lock);
            h = s->peer;
            pthread_mutex_unlock(&s->peer_lock);
            s->c.on_peer(&h, 0, s->c.ud);
        }
    }
}

int bl_peer(BlSession *s, BlinkHello *h)
{
    int up = atomic_load(&s->peer_up) && now_us() - atomic_load(&s->last_heard) <= PEER_GONE_US;
    if (h) {
        pthread_mutex_lock(&s->peer_lock);
        *h = s->peer;
        pthread_mutex_unlock(&s->peer_lock);
    }
    return up;
}

int bl_alive(BlSession *s) { return !atomic_load(&s->gone); }

/* ------------------------------------------------------------ sending */

int bl_send_audio(BlSession *s, int port, const float *stereo, int frames, double heard)
{
    if (!s || port < 0 || port >= BL_PORTS || frames <= 0)
        return 0;
    OutRing *o = &s->out[port];
    unsigned w = atomic_load_explicit(&o->w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&o->r, memory_order_acquire);
    if ((unsigned)frames > OUT_FRAMES - (w - r)) {
        atomic_fetch_add(&s->tx_drops, (unsigned)frames);
        sem_post(&s->wake);
        return 0;
    }
    for (int i = 0; i < frames; i++) {
        unsigned k = (w + (unsigned)i) & (OUT_FRAMES - 1);
        o->buf[2 * k] = stereo[2 * i], o->buf[2 * k + 1] = stereo[2 * i + 1];
    }
    atomic_store_explicit(&o->heard, heard, memory_order_relaxed);
    atomic_store_explicit(&o->w, w + (unsigned)frames, memory_order_release);
    sem_post(&s->wake);
    return 1;
}

void bl_send_midi(BlSession *s, int port, const uint8_t *b, int n)
{
    if (!s || n <= 0)
        return;
    pthread_mutex_lock(&s->mq_lock);
    while (n > 0) {
        int k = n > 1024 ? 1024 : n;
        if (s->mq_n + 3 + k > MIDI_Q)
            break; /* full: the rest is dropped */
        uint8_t *q = s->mq + s->mq_n;
        q[0] = (uint8_t)port, q[1] = (uint8_t)k, q[2] = (uint8_t)(k >> 8);
        memcpy(q + 3, b, (size_t)k);
        s->mq_n += 3 + k, b += k, n -= k;
    }
    pthread_mutex_unlock(&s->mq_lock);
    sem_post(&s->wake);
}

void bl_ping(BlSession *s)
{
    if (!s)
        return;
    atomic_store(&s->ping_out, now_us());
    sem_post(&s->wake);
}

/* sends what is in x: 0 sent or dropped, -2 the transport has gone */
static int flush(BlSession *s, Xfer *x)
{
    if (!x->n)
        return 0;
    int n = blink_pad(x->b, sizeof x->b, x->n);
    int r = s->t.send(s->t.ctx, x->b, n);
    x->n = 0;
    if (r == 1)
        atomic_fetch_add(&s->transfers_out, 1);
    else
        atomic_fetch_add(&s->send_fails, 1);
    return r == -2 ? -2 : 0;
}

/* one frame into the transfer, sending it first if it is full: 0, or -2 gone */
static int add(BlSession *s, Xfer *x, uint8_t type, uint8_t port, uint16_t seq, const void *p, int len)
{
    if (x->n + BLINK_HDR + len > CAP && flush(s, x) == -2)
        return -2;
    x->n += blink_frame(x->b + x->n, CAP - x->n, type, port, seq, p, len);
    return 0;
}

static void *tx_loop(void *arg)
{
    BlSession *s = arg;
    Xfer *xp = &s->x;
    uint8_t *pay = s->pay, *mq = s->mq_copy;
    float *tmp = s->tx_tmp;
    xp->n = 0;
    while (atomic_load(&s->running) && !atomic_load(&s->gone)) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 100000000;
        if (ts.tv_nsec >= 1000000000)
            ts.tv_sec++, ts.tv_nsec -= 1000000000;
        sem_timedwait(&s->wake, &ts);
        while (sem_trywait(&s->wake) == 0) {} /* one pass takes everything that has come */
        if (!atomic_load(&s->running))
            break;
        peer_check(s);
        int peer = bl_peer(s, NULL), r = 0;
        uint64_t t = now_us();

        if (t >= atomic_load(&s->hello_due)) {
            BlinkHello h;
            memset(&h, 0, sizeof h);
            h.version = BLINK_VERSION;
            snprintf(h.app, sizeof h.app, "%s", s->c.app ? s->c.app : "");
            snprintf(h.name, sizeof h.name, "%s", s->c.name ? s->c.name : "");
            memcpy(h.ver, s->c.ver, 3);
            h.nports = s->c.nports;
            memcpy(h.ports, s->c.ports, sizeof h.ports);
            int n = blink_hello_pack(pay, BLINK_MAX_PAYLOAD, &h);
            r |= add(s, xp, BLINK_HELLO, 0, s->misc_seq++, pay, n);
            atomic_store(&s->hello_due, t + HELLO_EVERY_US);
        }
        uint64_t v;
        if ((v = atomic_exchange(&s->pong_out, 0)))
            r |= add(s, xp, BLINK_PONG, 0, s->misc_seq++, &v, 8);
        if ((v = atomic_exchange(&s->ping_out, 0)) && peer)
            r |= add(s, xp, BLINK_PING, 0, s->misc_seq++, &v, 8);

        pthread_mutex_lock(&s->mq_lock);
        int mn = s->mq_n;
        memcpy(mq, s->mq, (size_t)mn);
        s->mq_n = 0;
        pthread_mutex_unlock(&s->mq_lock);
        for (int i = 0; i + 3 <= mn && peer;) {
            int len = mq[i + 1] | mq[i + 2] << 8;
            r |= add(s, xp, BLINK_MIDI, mq[i], s->midi_seq++, mq + i + 3, len);
            i += 3 + len;
        }

        for (int p = 0; p < BL_PORTS; p++) {
            OutRing *o = &s->out[p];
            unsigned w = atomic_load_explicit(&o->w, memory_order_acquire);
            unsigned rd = atomic_load_explicit(&o->r, memory_order_relaxed);
            if (!peer) { /* nobody to hear it: let it go */
                atomic_store_explicit(&o->r, w, memory_order_release);
                continue;
            }
            double heard = atomic_load_explicit(&o->heard, memory_order_relaxed);
            while (w != rd) {
                int k = (int)(w - rd) > CHUNK ? CHUNK : (int)(w - rd);
                for (int i = 0; i < k; i++) {
                    unsigned j = (rd + (unsigned)i) & (OUT_FRAMES - 1);
                    tmp[2 * i] = o->buf[2 * j], tmp[2 * i + 1] = o->buf[2 * j + 1];
                }
                rd += (unsigned)k;
                atomic_store_explicit(&o->r, rd, memory_order_release);
                int rate = s->c.rate ? s->c.rate : 48000;
                int n = blink_audio_pack(pay, BLINK_MAX_PAYLOAD, (uint32_t)rate, k, 2, heard, tmp);
                r |= add(s, xp, BLINK_AUDIO, (uint8_t)p, s->tx_seq[p]++, pay, n);
                atomic_fetch_add(&s->tx_frames, (unsigned)k);
            }
        }
        r |= flush(s, xp);
        if (r == -2)
            atomic_store(&s->gone, 1);
    }
    atomic_store(&s->tx_done, 1);
    return NULL;
}

/* ------------------------------------------------------------ receiving */

static void got(BlSession *s, const BlinkFrame *f)
{
    float *tmp = s->rx_tmp;
    uint64_t t = now_us();
    atomic_store(&s->last_heard, t);
    switch (f->type) {
    case BLINK_HELLO: {
        BlinkHello h;
        if (!blink_hello_read(f, &h))
            break;
        pthread_mutex_lock(&s->peer_lock);
        s->peer = h;
        pthread_mutex_unlock(&s->peer_lock);
        if (!atomic_exchange(&s->peer_up, 1)) {
            for (int p = 0; p < BL_PORTS; p++) {
                ARing *ring = atomic_load(&s->in[p]);
                if (ring)
                    ar_restart(ring); /* the audio starts over: no stale cushion from before */
                s->in_seen[p] = 0;
            }
            if (s->c.on_peer)
                s->c.on_peer(&h, 1, s->c.ud);
            atomic_store(&s->hello_due, 0); /* answer at once, so the other side hears of this one too */
            sem_post(&s->wake);
        }
        break;
    }
    case BLINK_AUDIO: {
        BlinkAudio a;
        if (f->port >= BL_PORTS || !blink_audio_read(f, &a) || !a.frames)
            break;
        int p = f->port;
        if (s->in_seen[p] && f->seq != s->in_seq[p])
            atomic_fetch_add(&s->rx_lost, (unsigned)(uint16_t)(f->seq - s->in_seq[p]) * a.frames);
        s->in_seen[p] = 1, s->in_seq[p] = (uint16_t)(f->seq + 1);
        ARing *ring = atomic_load(&s->in[p]);
        if (!ring) {
            ring = calloc(1, sizeof *ring);
            if (!ring)
                break;
            ar_init(ring);
            ring->rate = (int)a.rate;
            atomic_store(&s->in[p], ring);
        }
        blink_samples(&a, tmp);
        atomic_store(&s->in_rate[p], (int)a.rate);
        atomic_store(&s->in_block[p], (int)a.frames);
        atomic_store(&s->in_heard[p], a.heard);
        ar_push(ring, tmp, a.frames, a.channels, 0, now_ns());
        atomic_fetch_add(&s->rx_frames, a.frames);
        break;
    }
    case BLINK_MIDI:
        if (s->c.on_midi && f->len)
            s->c.on_midi(f->port, f->payload, f->len, t, s->c.ud);
        break;
    case BLINK_PING:
        if (f->len == 8) {
            uint64_t v;
            memcpy(&v, f->payload, 8);
            atomic_store(&s->pong_out, v ? v : 1);
            sem_post(&s->wake);
        }
        break;
    case BLINK_PONG:
        if (f->len == 8) {
            uint64_t v;
            memcpy(&v, f->payload, 8);
            if (v && v <= t)
                atomic_store(&s->rtt_us, (unsigned)(t - v));
        }
        break;
    default: /* NOP, or a type from a newer side */
        break;
    }
}

static void *rx_loop(void *arg)
{
    BlSession *s = arg;
    uint8_t *buf = s->rx_buf;
    while (atomic_load(&s->running)) {
        int n = s->t.recv(s->t.ctx, buf, BLINK_MAX_TRANSFER);
        if (n == -2) {
            atomic_store(&s->gone, 1);
            sem_post(&s->wake);
            break;
        }
        if (n < 0) { /* not connected now: try again shortly */
            peer_check(s);
            usleep(100000);
            continue;
        }
        if (n == 0) {
            peer_check(s);
            continue;
        }
        atomic_fetch_add(&s->transfers_in, 1);
        blink_rx_feed(&s->rx, buf, n);
        BlinkFrame f;
        while (blink_rx_next(&s->rx, &f))
            got(s, &f);
    }
    atomic_store(&s->rx_done, 1);
    return NULL;
}

int bl_pull(BlSession *s, int port, float *dst, int frames, double *heard)
{
    if (heard)
        *heard = -1;
    ARing *ring = s && port >= 0 && port < BL_PORTS ? atomic_load(&s->in[port]) : NULL;
    if (!ring) {
        memset(dst, 0, (size_t)frames * 2 * sizeof(float));
        return 0;
    }
    int block = atomic_load(&s->in_block[port]);
    int r = ar_pull(ring, dst, frames, block > 0 ? block : 256, now_ns());
    if (heard && r)
        *heard = atomic_load(&s->in_heard[port]);
    return r;
}

/* ------------------------------------------------------------ the session */

BlSession *bl_start(const BlConfig *c, BlTransport t)
{
    BlSession *s = calloc(1, sizeof *s);
    if (!s) {
        if (t.close) t.close(t.ctx);
        return NULL;
    }
    s->c = *c;
    if (s->c.nports > BLINK_MAX_PORTS)
        s->c.nports = BLINK_MAX_PORTS;
    s->t = t;
    sem_init(&s->wake, 0, 0);
    pthread_mutex_init(&s->mq_lock, NULL);
    pthread_mutex_init(&s->peer_lock, NULL);
    blink_rx_init(&s->rx);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal; /* no SA_RESTART: a blocked read or write returns, for bl_stop */
    sigaction(SIGUSR2, &sa, NULL);
    atomic_store(&s->running, 1);
    if (pthread_create(&s->rx_th, NULL, rx_loop, s) != 0) {
        bl_stop(s);
        return NULL;
    }
    if (pthread_create(&s->tx_th, NULL, tx_loop, s) != 0) {
        atomic_store(&s->tx_done, 1);
        s->tx_th = 0;
    }
    return s;
}

void bl_stop(BlSession *s)
{
    if (!s)
        return;
    atomic_store(&s->running, 0);
    sem_post(&s->wake);
    /* each thread may be blocked in its transport: a signal until it has seen running go to 0 */
    for (int i = 0; i < 300 && !(atomic_load(&s->rx_done) && atomic_load(&s->tx_done)); i++) {
        if (s->rx_th && !atomic_load(&s->rx_done)) pthread_kill(s->rx_th, SIGUSR2);
        if (s->tx_th && !atomic_load(&s->tx_done)) pthread_kill(s->tx_th, SIGUSR2);
        usleep(10000);
    }
    if (s->rx_th) pthread_join(s->rx_th, NULL);
    if (s->tx_th) pthread_join(s->tx_th, NULL);
    if (s->t.close)
        s->t.close(s->t.ctx);
    for (int p = 0; p < BL_PORTS; p++)
        free(atomic_load(&s->in[p]));
    sem_destroy(&s->wake);
    pthread_mutex_destroy(&s->mq_lock);
    pthread_mutex_destroy(&s->peer_lock);
    free(s);
}

void bl_stats(BlSession *s, int port, BlStats *st)
{
    memset(st, 0, sizeof *st);
    if (!s)
        return;
    st->peer = bl_peer(s, NULL);
    st->tx_frames = atomic_load(&s->tx_frames), st->tx_drops = atomic_load(&s->tx_drops);
    st->rx_frames = atomic_load(&s->rx_frames), st->rx_lost = atomic_load(&s->rx_lost);
    st->rx_bad = s->rx.bad;
    st->transfers_out = atomic_load(&s->transfers_out), st->transfers_in = atomic_load(&s->transfers_in);
    st->send_fails = atomic_load(&s->send_fails);
    st->rtt_ms = atomic_load(&s->rtt_us) / 1000.0f;
    ARing *ring = port >= 0 && port < BL_PORTS ? atomic_load(&s->in[port]) : NULL;
    if (ring) {
        st->underruns = atomic_load(&ring->underruns), st->skips = atomic_load(&ring->skips);
        st->cushion = atomic_load(&ring->fill), st->drift_ppm = atomic_load(&ring->ppm);
        st->rate = atomic_load(&s->in_rate[port]);
    }
}

/* ------------------------------------------------------------ USB: the follower */

typedef struct {
    int out_fd, in_fd;
} Follower;

static int f_send(void *ctx, const uint8_t *b, int n)
{
    Follower *f = ctx;
    ssize_t r = write(f->in_fd, b, (size_t)n);
    if (r == n) return 1;
    if (r < 0 && errno == EINTR) return 0;
    return -1; /* the leader isn't there (ESHUTDOWN): the next write waits for one */
}

static int f_recv(void *ctx, uint8_t *b, int max)
{
    Follower *f = ctx;
    ssize_t r = read(f->out_fd, b, (size_t)max);
    if (r > 0) return (int)r;
    if (r < 0 && errno == EINTR) return 0;
    return -1;
}

static void f_close(void *ctx)
{
    Follower *f = ctx;
    close(f->out_fd), close(f->in_fd);
    free(f);
}

int bl_usb_follower(BlTransport *t)
{
    int o, i;
    if (!link_data_open(&o, &i))
        return 0;
    Follower *f = malloc(sizeof *f);
    if (!f) { close(o), close(i); return 0; }
    f->out_fd = o, f->in_fd = i;
    t->send = f_send, t->recv = f_recv, t->close = f_close, t->ctx = f;
    return 1;
}

/* ------------------------------------------------------------ USB: the leader */

static int get(const char *path, char *buf, int n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return 0;
    ssize_t r = read(fd, buf, (size_t)n - 1);
    close(fd);
    if (r < 0) r = 0;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' ')) buf[--r] = 0;
    return r > 0;
}

static int hexfile(const char *dir, const char *name)
{
    char p[1024], v[32];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return get(p, v, sizeof v) ? (int)strtol(v, NULL, 16) : -1;
}

int bl_find(const char *sysfs, BlinkDev *out, int max)
{
    DIR *d = opendir(sysfs);
    if (!d) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        if (e->d_name[0] == '.' || strchr(e->d_name, ':')) continue;
        char dev[600];
        snprintf(dev, sizeof dev, "%s/%s", sysfs, e->d_name);
        if (hexfile(dev, "idVendor") != LINK_VID || hexfile(dev, "idProduct") != LINK_PID) continue;
        /* the data interface, whichever number it has */
        DIR *di = opendir(dev);
        struct dirent *g;
        while (di && (g = readdir(di)) && n < max) {
            char in[900];
            if (strncmp(g->d_name, e->d_name, strlen(e->d_name)) || !strchr(g->d_name, ':')) continue;
            snprintf(in, sizeof in, "%s/%s", dev, g->d_name);
            if (hexfile(in, "bInterfaceClass") != USB_CLASS_VENDOR_SPEC || hexfile(in, "bInterfaceSubClass") != BLINK_SUBCLASS ||
                hexfile(in, "bInterfaceProtocol") != BLINK_PROTOCOL)
                continue;
            int eo = -1, ei = -1;
            DIR *de = opendir(in);
            struct dirent *h;
            while (de && (h = readdir(de))) {
                if (strncmp(h->d_name, "ep_", 3)) continue;
                char ep[1200], dir[16] = "", type[16] = "", p[1300];
                snprintf(ep, sizeof ep, "%s/%s", in, h->d_name);
                snprintf(p, sizeof p, "%s/direction", ep), get(p, dir, sizeof dir);
                snprintf(p, sizeof p, "%s/type", ep), get(p, type, sizeof type);
                if (strcmp(type, "Bulk")) continue;
                int a = hexfile(ep, "bEndpointAddress");
                if (!strcmp(dir, "out")) eo = a;
                else if (!strcmp(dir, "in")) ei = a;
            }
            if (de) closedir(de);
            int ifn = hexfile(in, "bInterfaceNumber");
            char v[32], p[1024];
            int bus = 0, dn = 0;
            snprintf(p, sizeof p, "%s/busnum", dev);
            if (get(p, v, sizeof v)) bus = atoi(v);
            snprintf(p, sizeof p, "%s/devnum", dev);
            if (get(p, v, sizeof v)) dn = atoi(v);
            if (eo < 0 || ei < 0 || ifn < 0 || bus <= 0 || dn <= 0) continue;
            out[n].bus = bus, out[n].dev = dn, out[n].iface = ifn, out[n].ep_out = eo, out[n].ep_in = ei;
            if (snprintf(out[n].path, sizeof out[n].path, "%s", dev) < (int)sizeof out[n].path)
                n++;
        }
        if (di) closedir(di);
    }
    closedir(d);
    return n;
}

#define URBS 2
typedef struct {
    int fd, iface, ep_out;
    struct usbdevfs_urb urb[URBS];
    uint8_t buf[URBS][BLINK_MAX_TRANSFER];
    int queued[URBS];
} Leader;

static int gone_err(int e) { return e == ENODEV || e == ESHUTDOWN || e == ENOENT; }

static int l_submit(Leader *l, int i)
{
    if (ioctl(l->fd, USBDEVFS_SUBMITURB, &l->urb[i]) < 0)
        return gone_err(errno) ? -2 : -1;
    l->queued[i] = 1;
    return 0;
}

static int l_send(void *ctx, const uint8_t *b, int n)
{
    Leader *l = ctx;
    struct usbdevfs_bulktransfer bt = {.ep = (unsigned)l->ep_out, .len = (unsigned)n, .timeout = 50, .data = (void *)b};
    if (ioctl(l->fd, USBDEVFS_BULK, &bt) >= 0) return 1;
    return gone_err(errno) ? -2 : 0;
}

static int l_recv(void *ctx, uint8_t *b, int max)
{
    Leader *l = ctx;
    for (int i = 0; i < URBS; i++)
        if (!l->queued[i] && l_submit(l, i) == -2) return -2;
    struct usbdevfs_urb *u = NULL;
    if (ioctl(l->fd, USBDEVFS_REAPURB, &u) < 0)
        return errno == EINTR ? 0 : gone_err(errno) ? -2 : -1;
    int i = (int)(u - l->urb);
    if (i < 0 || i >= URBS) return 0;
    l->queued[i] = 0;
    int n = 0;
    if (u->status == 0 && u->actual_length > 0 && u->actual_length <= max) {
        memcpy(b, l->buf[i], (size_t)u->actual_length);
        n = u->actual_length;
    }
    else if (gone_err(-u->status))
        return -2;
    int r = l_submit(l, i); /* straight back in the queue: there is always a read waiting */
    return r == -2 ? -2 : n;
}

static void l_close(void *ctx)
{
    Leader *l = ctx;
    for (int i = 0; i < URBS; i++)
        if (l->queued[i]) ioctl(l->fd, USBDEVFS_DISCARDURB, &l->urb[i]);
    struct usbdevfs_urb *u;
    for (int k = 0; k < 50; k++) {
        int any = 0;
        for (int i = 0; i < URBS; i++) any |= l->queued[i];
        if (!any) break;
        if (ioctl(l->fd, USBDEVFS_REAPURBNDELAY, &u) == 0 && u >= l->urb && u < l->urb + URBS)
            l->queued[u - l->urb] = 0;
        else
            usleep(2000);
    }
    unsigned iface = (unsigned)l->iface;
    ioctl(l->fd, USBDEVFS_RELEASEINTERFACE, &iface);
    close(l->fd);
    free(l);
}

int bl_usb_leader(const BlinkDev *d, BlTransport *t)
{
    char dev[64];
    snprintf(dev, sizeof dev, "/dev/bus/usb/%03d/%03d", d->bus, d->dev);
    int fd = open(dev, O_RDWR | O_CLOEXEC);
    if (fd < 0) return 0;
    unsigned iface = (unsigned)d->iface;
    if (ioctl(fd, USBDEVFS_CLAIMINTERFACE, &iface) < 0) { close(fd); return 0; }
    Leader *l = calloc(1, sizeof *l);
    if (!l) { close(fd); return 0; }
    l->fd = fd, l->iface = d->iface, l->ep_out = d->ep_out;
    for (int i = 0; i < URBS; i++) {
        l->urb[i].type = USBDEVFS_URB_TYPE_BULK;
        l->urb[i].endpoint = (unsigned char)d->ep_in;
        l->urb[i].buffer = l->buf[i];
        l->urb[i].buffer_length = BLINK_MAX_TRANSFER;
    }
    t->send = l_send, t->recv = l_recv, t->close = l_close, t->ctx = l;
    return 1;
}
