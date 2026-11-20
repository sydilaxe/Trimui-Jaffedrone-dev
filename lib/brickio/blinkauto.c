/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Brick link's data channel, managed: see blinkauto.h. */
#define _GNU_SOURCE
#include "blinkauto.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static BlConfig cfg;
static char sysfs_dir[256];
static int leader;
static _Atomic(BlSession *) slot[BLA_MAX];
static char slot_path[BLA_MAX][300]; /* the leader's: each session's sysfs folder (the manager's thread only) */
static atomic_int in_audio;          /* the audio thread inside bla_send / bla_pull: a session isn't stopped under it */
static pthread_t thr;
static atomic_int running;
static pthread_mutex_t add_mx = PTHREAD_MUTEX_INITIALIZER;

static int add(BlTransport t, const char *path)
{
    pthread_mutex_lock(&add_mx);
    int k = -1;
    for (int i = 0; i < BLA_MAX && k < 0; i++)
        if (!atomic_load(&slot[i])) k = i;
    BlSession *s = k >= 0 ? bl_start(&cfg, t) : NULL;
    if (s) {
        snprintf(slot_path[k], sizeof slot_path[k], "%s", path ? path : "");
        atomic_store(&slot[k], s);
    }
    else if (t.close) t.close(t.ctx);
    pthread_mutex_unlock(&add_mx);
    return s != NULL;
}

int bla_add(BlTransport t) { return add(t, NULL); }

/* out of the slot first, then stopped once the audio thread can't be using it */
static void drop(int k)
{
    BlSession *s = atomic_exchange(&slot[k], NULL);
    if (!s) return;
    while (atomic_load(&in_audio)) usleep(200);
    bl_stop(s);
    slot_path[k][0] = 0;
}

static void *loop(void *arg)
{
    (void)arg;
    while (atomic_load(&running)) {
        for (int i = 0; i < BLA_MAX; i++) {
            BlSession *s = atomic_load(&slot[i]);
            if (s && !bl_alive(s)) {
                drop(i);
                fprintf(stderr, "blinkauto: a session's Brick has gone\n");
            }
        }
        if (leader) {
            BlinkDev d[BLA_MAX];
            int n = bl_find(sysfs_dir, d, BLA_MAX);
            for (int k = 0; k < n; k++) {
                int known = 0;
                for (int i = 0; i < BLA_MAX; i++)
                    known |= atomic_load(&slot[i]) && !strcmp(slot_path[i], d[k].path);
                BlTransport t;
                if (!known && bl_usb_leader(&d[k], &t) && add(t, d[k].path))
                    fprintf(stderr, "blinkauto: sending to the Brick at %s\n", d[k].path);
            }
        }
        else {
            int any = 0;
            for (int i = 0; i < BLA_MAX; i++) any |= atomic_load(&slot[i]) != NULL;
            BlTransport t;
            if (!any && bl_usb_follower(&t) && add(t, "follower"))
                fprintf(stderr, "blinkauto: the data channel is open, for the leader's sound\n");
        }
        for (int i = 0; i < 10 && atomic_load(&running); i++) usleep(100000); /* a second, stopping at once */
    }
    return NULL;
}

void bla_init(const BlConfig *c) { cfg = *c; }

static int start(const BlConfig *c, const char *sysfs, int lead)
{
    if (atomic_load(&running)) return -1;
    bla_init(c);
    leader = lead;
    snprintf(sysfs_dir, sizeof sysfs_dir, "%s", sysfs ? sysfs : "/sys/bus/usb/devices");
    atomic_store(&running, 1);
    if (pthread_create(&thr, NULL, loop, NULL) != 0) {
        atomic_store(&running, 0);
        return -1;
    }
    return 0;
}

int bla_lead_start(const BlConfig *c, const char *sysfs) { return start(c, sysfs, 1); }
int bla_follow_start(const BlConfig *c) { return start(c, NULL, 0); }

void bla_stop(void)
{
    if (atomic_exchange(&running, 0)) pthread_join(thr, NULL);
    for (int i = 0; i < BLA_MAX; i++) drop(i);
}

void bla_send(int port, const float *stereo, int frames, double heard)
{
    atomic_fetch_add(&in_audio, 1);
    for (int i = 0; i < BLA_MAX; i++) {
        BlSession *s = atomic_load(&slot[i]);
        if (s) bl_send_audio(s, port, stereo, frames, heard);
    }
    atomic_fetch_sub(&in_audio, 1);
}

int bla_pull(int port, float *dst, int frames, double *heard)
{
    int got = 0;
    atomic_fetch_add(&in_audio, 1);
    for (int i = 0; i < BLA_MAX && !got; i++) {
        BlSession *s = atomic_load(&slot[i]);
        if (s && bl_peer(s, NULL)) got = bl_pull(s, port, dst, frames, heard);
    }
    atomic_fetch_sub(&in_audio, 1);
    if (!got) {
        memset(dst, 0, sizeof(float) * 2 * (size_t)frames);
        if (heard) *heard = -1.0;
    }
    return got;
}

int bla_sessions(void)
{
    int n = 0;
    for (int i = 0; i < BLA_MAX; i++) n += atomic_load(&slot[i]) != NULL;
    return n;
}

int bla_peers(BlinkHello *first)
{
    int n = 0;
    for (int i = 0; i < BLA_MAX; i++) {
        BlSession *s = atomic_load(&slot[i]);
        BlinkHello h;
        if (s && bl_peer(s, &h)) {
            if (!n && first) *first = h;
            n++;
        }
    }
    return n;
}
