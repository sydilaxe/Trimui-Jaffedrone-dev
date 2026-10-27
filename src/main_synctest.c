/* Host-side wifi sync interop test: run two of these (or one HI5 and one
 * Jaffedrone build) on the same machine / LAN.  usage: synctest LEAD|FOLLOW|AUTO BPM SECONDS */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "engine.h"
#include "sync.h"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

int main(int argc, char **argv)
{
    int mode = argc > 1 && !strcmp(argv[1], "LEAD") ? SYNC_LEAD : argc > 1 && !strcmp(argv[1], "FOLLOW") ? SYNC_FOLLOW : SYNC_AUTO;
    float bpm = argc > 2 ? (float)atof(argv[2]) : 120.f;
    int secs = argc > 3 ? atoi(argv[3]) : 5;
    song_default();
    engine_init(48000);
    song.gp[GP_SYNC] = mode;
    engine_set_bpm(bpm);
    engine_set_playing(mode == SYNC_LEAD);
    sync_init();
    static int16_t pcm[2 * 480];
    uint64_t t0 = now_us();
    while (now_us() - t0 < (uint64_t)secs * 1000000ULL) {
        engine_render(pcm, 480);
        sync_poll(now_us());
        usleep(10000);
    }
    const SyncStatus *s = sync_status();
    printf("JAFFEDRONE %s: bpm %.1f playing %d peers %d leading %d following %d offset %+.1f ms pos %.2f\n",
           argv[1], engine_bpm(), live.playing, s->peers, s->leading, s->have_leader, s->offset_ms, engine_position());
    sync_close();
    return 0;
}
