/* AIR stress sweep: many random combinations, checked for runaway feedback.
 * Each: 20 s of play, then every exciter stops (players muted, bows and
 * breath to zero, rhythms off) and 20 s more. Judged on the voices' own
 * levels (the meters), which the leveller does not touch:
 *   no runaway: after the stop, the voices never pass 1.5 times their level
 *            while playing (a runaway ends louder than anything played), and
 *            never NaN. Listeners singing louder than a quiet player, or a
 *            long-ringing listener still filling from a struck player's ring,
 *            are the effect, not a fault.
 * usage: air_sweep N [seed] */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "engine.h"
extern float air_k[3], air_send[3], engine_vout_pk[NS];
extern int engine_listen, engine_air_mode;
#define R 48000
static int16_t pcm[R / 4 * 2];
static unsigned rs = 1;
static int rnd(int n) { rs = rs * 1103515245u + 12345u; return (int)((rs >> 16) % (unsigned)n); }
typedef struct { int v[NS], player[NS], pan_same, unison, symp, decay_top, room; float air; } Cfg;
static void setup(const Cfg *c)
{
    song_default();
    for (int s = 0; s < NS; s++) {
        song.on[s] = 1;
        song.sp[s][SP_VOICE] = c->v[s];
        if (c->pan_same) song.sp[s][SP_PAN] = 0;
        if (c->unison) song.sp[s][SP_NOTE] = song.sp[0][SP_NOTE];
        if (c->decay_top) song.sp[s][SP_DECAY] = 100;
        if (!c->player[s]) song.sp[s][SP_DECAY] = 100;
    }
    if (c->symp) { song.fx[FX_SYMP] = fparam_def(FX_SYMP)->max; song.fx[FX_SCOUNT] = fparam_def(FX_SCOUNT)->max; }
}
/* returns: max voice meter while playing; fills the totals after the stop */
static float run(const Cfg *c, float air, float room, float *after3, float *after10, float *after_max, int *nan)
{
    setup(c);
    (void)room;
    song.fx[FX_AIR] = (int)(air * 100.f + 0.5f);
    for (int s = 0; s < NS; s++)
        song.sp[s][SP_LISTEN] = !c->player[s]; /* the rest only listen */
    engine_init(R);
    engine_set_playing(1);
    float mx = 0;
    *nan = 0;
    for (int q = 0; q < 80; q++) { /* 20 s in quarter seconds */
        memset(engine_vout_pk, 0, sizeof engine_vout_pk);
        engine_render(pcm, R / 4);
        float t = 0;
        for (int s = 0; s < NS; s++) {
            if (!(engine_vout_pk[s] == engine_vout_pk[s])) *nan = 1;
            t += engine_vout_pk[s];
        }
        if (q >= 20) mx += t / 60; /* the voices' level, all together, averaged over 5-20 s (a held wall counts, not only peaks) */
    }
    for (int s = 0; s < NS; s++) { /* everything stops being played */
        if (c->player[s]) song.on[s] = 0;
        song.sp[s][SP_BOW] = 0; song.sp[s][SP_PON] = song.sp[s][SP_BON] = 0;
    }
    *after3 = *after10 = *after_max = 0;
    for (int q = 0; q < 80; q++) {
        memset(engine_vout_pk, 0, sizeof engine_vout_pk);
        engine_render(pcm, R / 4);
        float t = 0;
        for (int s = 0; s < NS; s++) {
            if (!(engine_vout_pk[s] == engine_vout_pk[s])) *nan = 1;
            t += engine_vout_pk[s];
        }
        if (t > *after_max) *after_max = t;
        if (q >= 20 && q < 28) *after3 += t / 8; /* seconds 5-7 after the stop, averaged (pairs beat) */
        if (q >= 68) *after10 += t / 12;         /* seconds 17-20 */
    }
    return mx;
}
int main(int argc, char **argv)
{
    int n = argc > 1 ? atoi(argv[1]) : 100;
    rs = argc > 2 ? (unsigned)atoi(argv[2]) : 1;
    air_k[0] = argc > 3 ? atof(argv[3]) : 1; air_k[1] = 2; air_k[2] = 2;
    air_send[0] = argc > 4 ? atof(argv[4]) : 1; air_send[2] = 0.3f;
    const float RATIO = 3.f;
    float room_max = argc > 5 ? atof(argv[5]) : 1.f;
    int fails = 0;
    double worst_ratio = 0, worst_growth = 0;
    int nv = 1 + NEXCITE + NPERC;
    for (int i = 0; i < n; i++) {
        Cfg c;
        memset(&c, 0, sizeof c);
        int kind = 1 + rnd(2); /* 1: one plays, 2: some play (at least one) */
        for (int s = 0; s < NS; s++) {
            c.v[s] = rnd(nv);
            c.player[s] = kind == 1 ? s == 0 : rnd(2);
        }
        c.player[rnd(NS)] = 1;
        c.pan_same = rnd(3) == 0;
        c.unison = rnd(3) == 0;
        c.symp = rnd(2);
        c.decay_top = rnd(2);
        float air = rnd(2) ? 1.f : 0.5f, room = rnd(2) ? room_max : 0.f;
        float a3, a10, b3, b10, amax, bmax;
        int nan0, nan1;
        float off = run(&c, 0, 0, &b3, &b10, &bmax, &nan0);
        float on = run(&c, air, room, &a3, &a10, &amax, &nan1);
        double ratio = on / (off + 1e-3), growth = a10 / (a3 + 1e-4), growth_off = b10 / (b3 + 1e-4);
        /* growth after the stop counts only beyond what the same voices do without AIR */
        int grows = a3 > 0.003f && growth > 1.02 && growth > growth_off * 1.02;
        /* a runaway ends louder than anything heard while playing: after the
         * stop nothing may pass 1.5 times the level while playing (with AIR) */
        int runaway = amax > 1.5f * on + 0.01f;
        (void)grows; (void)RATIO;
        int bad = nan1 || runaway;
        if (ratio > worst_ratio) worst_ratio = ratio;
        if (a3 > 0.003f && growth / (growth_off > 1 ? growth_off : 1) > worst_growth) worst_growth = growth / (growth_off > 1 ? growth_off : 1);
        if (bad) {
            fails++;
            printf("FAIL %3d: voices %s %s %s %s %s players %d%d%d%d%d pan_same %d unison %d symp %d decay %d air %.1f room %.0f: "
                   "level off %.3f on %.3f (x%.1f), after stop %.3f -> %.3f, loudest after stop %.3f%s\n",
                   i, engine_voice_name(c.v[0]), engine_voice_name(c.v[1]), engine_voice_name(c.v[2]),
                   engine_voice_name(c.v[3]), engine_voice_name(c.v[4]), c.player[0], c.player[1], c.player[2],
                   c.player[3], c.player[4], c.pan_same, c.unison, c.symp, c.decay_top, air, room, off, on, ratio, a3,
                   a10, amax, nan1 ? " NaN" : "");
        }
        fflush(stdout);
    }
    printf("sweep: %d combinations, %d failed; worst loudness ratio x%.2f, worst growth after the stop x%.3f\n", n, fails,
           worst_ratio, worst_growth);
    return 0;
}
