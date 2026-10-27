#include "ahead.h"

#include <pthread.h>
#include <sched.h>
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

int ahead_start(void (*render)(int16_t *, int), int rate, int chunk, int ahead, int rt)
{
    render_ = render;
    rate_ = rate;
    chunk_ = chunk < 16 ? 16 : chunk > 4096 ? 4096 : chunk;
    ahead_ = ahead < chunk_ ? chunk_ : ahead > RING / 2 ? RING / 2 : ahead;
    wpos = rpos = 0;
    memset(&st, 0, sizeof st);
    /* the engine's lock passes on priority: a setting change in the screen's
     * thread never keeps the sound waiting behind it */
    static int eng_ready;
    if (!eng_ready) {
        pthread_mutexattr_t ma;
        pthread_mutexattr_init(&ma);
        pthread_mutexattr_setprotocol(&ma, PTHREAD_PRIO_INHERIT);
        pthread_mutex_init(&eng, &ma);
        pthread_mutexattr_destroy(&ma);
        eng_ready = 1;
    }
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
    pthread_cond_signal(&want);
    pthread_mutex_unlock(&mx);
    pthread_join(th, NULL);
}

int ahead_rt(void) { return rt_ok; }

int ahead_ready(void)
{
    pthread_mutex_lock(&mx);
    int n = (int)(wpos - rpos);
    pthread_mutex_unlock(&mx);
    return n;
}

void ahead_read(int16_t *out, int frames)
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
}

void ahead_lock(void) { pthread_mutex_lock(&eng); }
void ahead_unlock(void) { pthread_mutex_unlock(&eng); }

void ahead_take_stats(AheadStats *s)
{
    *s = st;
    memset(&st, 0, sizeof st);
}
