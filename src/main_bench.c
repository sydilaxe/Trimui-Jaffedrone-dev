/* The load of many voice mixtures, each at its worst: every effect at its
 * top, each voice at full bow or breath, ring, sizzle, fold and stretch,
 * the fastest tempo. Prints them from heaviest to lightest, relative to the
 * plain default drone, so the ranking carries over from this machine to the
 * Miyoo (its numbers do not). With an ARM build it runs on the device too.
 *
 * usage: jd_bench [seconds per mixture, default 6] */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "engine.h"

#define RATE 48000
#define S(p) VOICE_PERC(PC_##p)
#define W(e) (1 + EX_##e)

typedef struct {
    const char *name;
    int v[NS];
} Mix;

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static int16_t pcm[2 * 1024];

static double run(const int *v, int worst, double secs)
{
    double best = 1e9;
    for (int rep = 0; rep < 3; rep++) { /* the fastest of three: the least disturbed */
        song_default();
        if (worst) {
            for (int p = 0; p < NFX; p++)
                song.fx[p] = fparam_def(p)->max;
            song.gp[GP_SUB] = gparam_def(GP_SUB)->max;
            song.gp[GP_SUBRES] = gparam_def(GP_SUBRES)->max;
            song.gp[GP_BPM] = gparam_def(GP_BPM)->max;
        }
        for (int s = 0; s < NS && v; s++) {
            song.on[s] = 1;
            song.sp[s][SP_VOICE] = v[s];
            static const int top[] = {SP_BOW, SP_DECAY, SP_BUZZ, SP_FOLD, SP_STIFF, SP_BRIGHT};
            for (int k = 0; worst && k < 6; k++)
                song.sp[s][top[k]] = sparam_def(top[k])->max;
        }
        engine_init(RATE);
        engine_set_playing(1);
        for (int i = 0; i < RATE / 1024; i++) /* a second to settle */
            engine_render(pcm, 1024);
        double t0 = now_s();
        int n = (int)(secs * RATE / 1024);
        for (int i = 0; i < n; i++)
            engine_render(pcm, 1024);
        double t = (now_s() - t0) / (n * 1024.0 / RATE);
        if (t < best)
            best = t;
    }
    return best;
}

int main(int argc, char **argv)
{
    double secs = argc > 1 ? atof(argv[1]) : 6.0;
    static const Mix mixes[] = {
        {"2 cymbals, 2 flutes, free reed", {S(CYMBAL), S(CYMBAL), W(FLUTE), W(FLUTE), W(FREE)}},
        {"3 cymbals, 2 free reeds", {S(CYMBAL), S(CYMBAL), S(CYMBAL), W(FREE), W(FREE)}},
        {"4 cymbals, free reed", {S(CYMBAL), S(CYMBAL), S(CYMBAL), S(CYMBAL), W(FREE)}},
        {"5 free reeds and flutes", {W(FREE), W(FLUTE), W(FREE), W(FLUTE), W(FREE)}},
        {"string, free reed, cymbal, flute, gong", {0, W(FREE), S(CYMBAL), W(FLUTE), S(GONG)}},
        {"gamelan: gender, bonang, gong, 2 strings", {S(GENDER), S(BONANG), S(GONG), 0, 0}},
        {"2 strings, reed, gender, bowl", {0, 0, W(REED), S(GENDER), S(BOWL)}},
        {"bowl, glass, alien, chord, cymbal", {S(BOWL), S(GLASS), S(ALIEN), S(CHORD), S(CYMBAL)}},
        {"handpan, steelpan, tongue, drum, flute", {S(HANDPAN), S(STEELPAN), S(TONGUE), S(DRUM), W(FLUTE)}},
        {"bansuri, recorder, double, lip, string", {W(BANSURI), W(RECORDER), W(DOUBLE), W(LIP), 0}},
        {"3 strings, 2 cymbals", {0, 0, 0, S(CYMBAL), S(CYMBAL)}},
        {"one of each family, twice cymbal", {0, W(FLUTE), S(CYMBAL), W(FREE), S(CYMBAL)}},
    };
    int nmix = (int)(sizeof mixes / sizeof mixes[0]);
    int nv = 1 + NEXCITE + NPERC, total = nv + nmix;
    const char **names = malloc(sizeof(char *) * total);
    double *load = malloc(sizeof(double) * total);
    char(*buf)[48] = malloc(48 * nv);
    double base = run(NULL, 0, secs);
    for (int k = 0; k < nv; k++) {
        int v[NS];
        for (int s = 0; s < NS; s++)
            v[s] = k;
        snprintf(buf[k], 48, "5 x %s", engine_voice_name(k));
        names[k] = buf[k];
        load[k] = run(v, 1, secs);
        fprintf(stderr, ".");
    }
    for (int m = 0; m < nmix; m++) {
        names[nv + m] = mixes[m].name;
        load[nv + m] = run(mixes[m].v, 1, secs);
        fprintf(stderr, ".");
    }
    fprintf(stderr, "\n");
    for (int i = 0; i < total; i++) /* heaviest first */
        for (int j = i + 1; j < total; j++)
            if (load[j] > load[i]) {
                double t = load[i];
                load[i] = load[j], load[j] = t;
                const char *n = names[i];
                names[i] = names[j], names[j] = n;
            }
    printf("jaffedrone v%s: load at its worst (everything up), relative to the default drone (%.2f%% of real time here)\n",
           JD_VERSION, base * 100.0);
    for (int i = 0; i < total; i++)
        printf("%5.2fx  %5.2f%%  %s\n", load[i] / base, load[i] * 100.0, names[i]);
    return 0;
}
