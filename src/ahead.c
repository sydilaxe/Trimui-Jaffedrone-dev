#define _GNU_SOURCE
#include "ahead.h"
#include "aclock.h"
#include "engine.h"

#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <time.h>

#define RING (16384) /* frames, a power of two */

static int16_t ring[RING * 2];
static volatile uint32_t wpos, rpos; /* frames written and read, ever */
static int chunk_, ahead_, rate_, rt_ok;
static void (*render_)(int16_t *, int);
static pthread_t th;
static pthread_mutex_t eng; /* the engine: the thread and setting changes (set up on the first start) */
static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;  /* the ring's waiting */
static pthread_cond_t want = PTHREAD_COND_INITIALIZER, have = PTHREAD_COND_INITIALIZER;
static volatile int running;
static AheadStats st;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* split: the voices and the effects on two threads (two cores), a block
 * apart; NBLK blocks pass between them */
#define NBLK 3
static int split;
static EngineBlock *blk[NBLK];
static volatile uint32_t vdone, edone, vpos; /* blocks made by each stage; frames the voices have made */
static pthread_t th_e;
static pthread_mutex_t eng_fx; /* the effects stage's own lock */
static pthread_cond_t ready = PTHREAD_COND_INITIALIZER; /* a block of voices for the effects */
static int core_v = -1, core_e = -1;

static void pin(int core)
{
    if (core < 0 || core >= sysconf(_SC_NPROCESSORS_CONF))
        return;
    cpu_set_t cs;
    CPU_ZERO(&cs);
    CPU_SET(core, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof cs, &cs);
}

static void count(uint32_t spent, volatile uint32_t *sum)
{
    *sum += spent;
    uint32_t scaled = (uint32_t)((uint64_t)spent * (uint32_t)ahead_ / (uint32_t)chunk_);
    if (scaled > st.us_max_buf)
        st.us_max_buf = scaled;
}

static void *loop_voices(void *arg)
{
    (void)arg;
    pin(core_v);
    while (running) {
        pthread_mutex_lock(&mx);
        while (running && (vdone - edone >= NBLK || vpos - rpos >= (uint32_t)ahead_))
            pthread_cond_wait(&want, &mx);
        uint32_t i = vdone;
        pthread_mutex_unlock(&mx);
        if (!running)
            break;
        uint64_t t0 = now_us();
        pthread_mutex_lock(&eng);
        engine_voices(blk[i % NBLK], chunk_);
        /* the voices move the music on; these frames reach the speaker in
         * this order, a block later, so the audio clock counts them here */
        aclock_made(chunk_, engine_position(), engine_steps_per_frame());
        pthread_mutex_unlock(&eng);
        count((uint32_t)(now_us() - t0), &st.us_voices);
        pthread_mutex_lock(&mx);
        vdone = i + 1;
        vpos += chunk_;
        pthread_cond_signal(&ready);
        pthread_mutex_unlock(&mx);
    }
    return NULL;
}

static void *loop_effects(void *arg)
{
    (void)arg;
    pin(core_e);
    static int16_t piece[4096 * 2];
    while (running) {
        pthread_mutex_lock(&mx);
        while (running && edone == vdone)
            pthread_cond_wait(&ready, &mx);
        uint32_t i = edone;
        pthread_mutex_unlock(&mx);
        if (!running)
            break;
        uint64_t t0 = now_us();
        pthread_mutex_lock(&eng_fx);
        engine_effects(blk[i % NBLK], piece);
        pthread_mutex_unlock(&eng_fx);
        count((uint32_t)(now_us() - t0), &st.us_effects);
        uint32_t w = wpos;
        for (int k = 0; k < chunk_; k++) {
            uint32_t r = (w + k) & (RING - 1);
            ring[2 * r] = piece[2 * k];
            ring[2 * r + 1] = piece[2 * k + 1];
        }
        __sync_synchronize(); /* the frames before the count that shows them */
        pthread_mutex_lock(&mx);
        wpos = w + chunk_;
        edone = i + 1;
        pthread_cond_signal(&have);
        pthread_cond_signal(&want); /* a block free again */
        pthread_mutex_unlock(&mx);
    }
    return NULL;
}


static void *loop(void *arg)
{
    (void)arg;
    static int16_t piece[4096 * 2];
    while (running) {
        pthread_mutex_lock(&mx);
        while (running && wpos - rpos >= (uint32_t)ahead_)
            pthread_cond_wait(&want, &mx);
        pthread_mutex_unlock(&mx);
        if (!running)
            break;
        uint64_t t0 = now_us();
        pthread_mutex_lock(&eng);
        render_(piece, chunk_);
        pthread_mutex_unlock(&eng);
        uint32_t spent = (uint32_t)(now_us() - t0);
        st.us_sum += spent;
        uint32_t scaled = (uint32_t)((uint64_t)spent * (uint32_t)ahead_ / (uint32_t)chunk_);
        if (scaled > st.us_max_buf)
            st.us_max_buf = scaled;
        uint32_t w = wpos;
        for (int i = 0; i < chunk_; i++) {
            uint32_t k = (w + i) & (RING - 1);
            ring[2 * k] = piece[2 * i];
            ring[2 * k + 1] = piece[2 * i + 1];
        }
        __sync_synchronize(); /* the frames before the count that shows them */
        pthread_mutex_lock(&mx);
        wpos = w + chunk_;
        pthread_cond_signal(&have);
        pthread_mutex_unlock(&mx);
    }
    return NULL;
}

static int start(void *(*fn)(void *), pthread_t *t, int rt, int prio)
{
    pthread_attr_t a;
    pthread_attr_init(&a);
    int ok = 0;
    if (rt) {
        struct sched_param sp = {.sched_priority = prio};
        pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&a, SCHED_FIFO);
        pthread_attr_setschedparam(&a, &sp);
        ok = pthread_create(t, &a, fn, NULL) == 0 ? 2 : 0;
    }
    if (!ok)
        ok = pthread_create(t, NULL, fn, NULL) == 0;
    pthread_attr_destroy(&a);
    return ok; /* 2: real time, 1: ordinary, 0: none */
}

static void init_lock(pthread_mutex_t *m)
{
    /* the engine's locks pass on priority: a setting change in the screen's
     * thread never keeps the sound waiting behind it */
    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(m, &ma);
    pthread_mutexattr_destroy(&ma);
}

static void init_locks(void)
{
    static int ready_;
    if (!ready_) {
        init_lock(&eng);
        init_lock(&eng_fx);
        ready_ = 1;
    }
}

int ahead_start_split(int rate, int chunk, int ahead, int rt, int cv, int ce)
{
    init_locks();
    rate_ = rate;
    chunk_ = chunk < 16 ? 16 : chunk > 4096 ? 4096 : chunk;
    ahead_ = ahead < chunk_ ? chunk_ : ahead > RING / 2 ? RING / 2 : ahead;
    for (int i = 0; i < NBLK; i++)
        if (!blk[i] && !(blk[i] = engine_block_new(4096)))
            return 0;
    wpos = rpos = vpos = vdone = edone = 0;
    memset(&st, 0, sizeof st);
    core_v = cv, core_e = ce;
    split = 1;
    running = 1;
    int a = start(loop_voices, &th, rt, 45);
    if (!a) {
        running = split = 0;
        return 0;
    }
    int b = start(loop_effects, &th_e, rt, 44);
    if (!b) {
        pthread_mutex_lock(&mx);
        running = 0;
        pthread_cond_broadcast(&want);
        pthread_mutex_unlock(&mx);
        pthread_join(th, NULL);
        split = 0;
        return 0;
    }
    rt_ok = a == 2 && b == 2;
    return 1;
}

int ahead_split(void) { return split && running; }

int ahead_start(void (*render)(int16_t *, int), int rate, int chunk, int ahead, int rt)
{
    split = 0;
    render_ = render;
    rate_ = rate;
    chunk_ = chunk < 16 ? 16 : chunk > 4096 ? 4096 : chunk;
    ahead_ = ahead < chunk_ ? chunk_ : ahead > RING / 2 ? RING / 2 : ahead;
    wpos = rpos = 0;
    memset(&st, 0, sizeof st);
    init_locks();
    running = 1;
    pthread_attr_t a;
    pthread_attr_init(&a);
    rt_ok = 0;
    if (rt) {
        struct sched_param sp = {.sched_priority = 45};
        pthread_attr_setinheritsched(&a, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&a, SCHED_FIFO);
        pthread_attr_setschedparam(&a, &sp);
        rt_ok = pthread_create(&th, &a, loop, NULL) == 0;
    }
    if (!rt_ok && pthread_create(&th, NULL, loop, NULL) != 0) {
        running = 0;
        pthread_attr_destroy(&a);
        return 0;
    }
    pthread_attr_destroy(&a);
    return 1;
}

void ahead_stop(void)
{
    if (!running)
        return;
    pthread_mutex_lock(&mx);
    running = 0;
    pthread_cond_broadcast(&want);
    pthread_cond_broadcast(&ready);
    pthread_mutex_unlock(&mx);
    pthread_join(th, NULL);
    if (split)
        pthread_join(th_e, NULL);
    split = 0;
}

int ahead_rt(void) { return rt_ok; }

int ahead_ready(void)
{
    pthread_mutex_lock(&mx);
    int n = (int)(wpos - rpos);
    pthread_mutex_unlock(&mx);
    return n;
}

int ahead_read(int16_t *out, int frames)
{
    /* wait for the frames, at most a buffer's time: past that the device
     * would run dry anyway, and the missing part is silence */
    pthread_mutex_lock(&mx);
    if (wpos - rpos < (uint32_t)frames) {
        pthread_cond_signal(&want);
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        long ns = dl.tv_nsec + (long)(1e9 * frames / (rate_ ? rate_ : 48000));
        dl.tv_sec += ns / 1000000000L;
        dl.tv_nsec = ns % 1000000000L;
        while (running && wpos - rpos < (uint32_t)frames)
            if (pthread_cond_timedwait(&have, &mx, &dl) != 0)
                break;
    }
    uint32_t avail = wpos - rpos;
    pthread_mutex_unlock(&mx);
    __sync_synchronize();
    int n = avail < (uint32_t)frames ? (int)avail : frames;
    uint32_t r = rpos;
    for (int i = 0; i < n; i++) {
        uint32_t k = (r + i) & (RING - 1);
        out[2 * i] = ring[2 * k];
        out[2 * i + 1] = ring[2 * k + 1];
    }
    if (n < frames) {
        memset(out + 2 * n, 0, (size_t)(frames - n) * 4);
        st.short_frames += (uint32_t)(frames - n);
        st.short_calls++;
    }
    pthread_mutex_lock(&mx);
    rpos = r + n;
    pthread_cond_signal(&want); /* room again: make more */
    pthread_mutex_unlock(&mx);
    return n;
}

/* both stages held, always in this order */
void ahead_lock(void)
{
    pthread_mutex_lock(&eng);
    if (split)
        pthread_mutex_lock(&eng_fx);
}
void ahead_unlock(void)
{
    if (split)
        pthread_mutex_unlock(&eng_fx);
    pthread_mutex_unlock(&eng);
}

void ahead_take_stats(AheadStats *s)
{
    *s = st;
    if (split) /* the busier of the two cores is the one that matters */
        s->us_sum = st.us_voices > st.us_effects ? st.us_voices : st.us_effects;
    memset(&st, 0, sizeof st);
}
