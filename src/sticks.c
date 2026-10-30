/* The Brick Pro's sticks and their lights: see sticks.h */
#include "sticks.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "log.h"

#define DEADZONE 0.12f

static float raw_x[STK_AXES];         /* -1 .. 1, as the stick sends it */
static float seen[STK_AXES][2], reach0 = 0.6f;

void sticks_reset(float reach)
{
    reach0 = reach < 0.3f ? 0.3f : reach > 1.f ? 1.f : reach;
    memset(raw_x, 0, sizeof raw_x);
    memset(seen, 0, sizeof seen);
}

void sticks_axis(int axis, int raw)
{
    if (axis < 0 || axis >= STK_AXES)
        return;
    float x = raw / 32767.f;
    x = x > 1.f ? 1.f : x < -1.f ? -1.f : x;
    raw_x[axis] = x;
    int dir = x > 0.f;
    if (fabsf(x) > seen[axis][dir])
        seen[axis][dir] = fabsf(x);
}

/* the stick at full: the start, or a little short of the furthest seen, so
 * it can be reached again without pressing hard into the gate */
float sticks_reach(int axis, int dir)
{
    float r = 0.96f * seen[axis][dir];
    return r > reach0 ? r : reach0;
}

static float dead(float x)
{
    float a = fabsf(x);
    if (a <= DEADZONE)
        return 0.f;
    a = (a - DEADZONE) / (1.f - DEADZONE);
    a = a > 1.f ? 1.f : a;
    return x < 0.f ? -a : a;
}

void sticks_read(float out[STK_AXES])
{
    for (int st = 0; st < 2; st++) {
        int ax = st * 2, ay = ax + 1;
        float x = raw_x[ax] / sticks_reach(ax, raw_x[ax] > 0.f), y = raw_x[ay] / sticks_reach(ay, raw_x[ay] > 0.f);
        /* a round gate to a square: a point keeps its direction, and its
         * distance from the centre becomes the larger of the two axes, so
         * the rim reaches 1 on both axes on a diagonal */
        float r = sqrtf(x * x + y * y), m = fmaxf(fabsf(x), fabsf(y));
        if (m > 1e-6f) {
            float k = r / m;
            x *= k, y *= k;
        }
        x = x > 1.f ? 1.f : x < -1.f ? -1.f : x;
        y = y > 1.f ? 1.f : y < -1.f ? -1.f : y;
        out[ax] = dead(x);
        out[ay] = dead(y);
    }
}

float stick_bow_pressure(float up)
{
    if (up <= 0.f)
        return 0.f;
    up = up > 1.f ? 1.f : up;
    return 0.1f + 1.4f * up * sqrtf(up);
}

int stick_control(int *held, int set, int min, int max, float amt)
{
    amt = amt > 1.f ? 1.f : amt < -1.f ? -1.f : amt;
    float t = set + amt * (amt > 0.f ? max - set : set - min);
    int want = (int)lrintf(t);
    want = want < min ? min : want > max ? max : want;
    /* the first value, or far enough from the one playing */
    if (*held < min || *held > max || fabsf(t - *held) > 0.8f || want == min || want == max)
        *held = want;
    return *held;
}

uint32_t stick_led_colour(uint32_t rgb, float intensity)
{
    if (intensity <= 0.f)
        return 0;
    intensity = intensity > 1.f ? 1.f : intensity;
    float k = powf(intensity, 1.8f);
    uint32_t r = (uint32_t)lrintf(((rgb >> 16) & 255) * k), g = (uint32_t)lrintf(((rgb >> 8) & 255) * k),
             b = (uint32_t)lrintf((rgb & 255) * k);
    return r << 16 | g << 8 | b;
}

/* ------------------------------------------------------------- lights
 * TrimUI's lights driver (led_anim, as TrimUI OS, NextUI and MustardOS drive
 * it): for each light, effect_rgb_hex_<name> takes a colour as six hex digits
 * and a space, effect_cycles_<name> how often to run the effect (-1: always),
 * effect_<name> the effect (4: a steady colour), and effect_enable 1 starts
 * them. max_scale is the brightness of them all, 0 .. 60. The sticks' lights
 * are "l" and "r". JD_LED_EFFECT changes the effect number for another
 * firmware.
 *
 * MustardOS sets max_scale to 0 when it starts, so the lights are dark until
 * something turns them up: while Jaffedrone runs, a brightness of 0 becomes
 * 60 (JD_LED_BRIGHTNESS sets another, 1 .. 60, whatever the system has), and
 * it is put back at the end. */
/* The Brick Pro: the driver's named lights (l, r, lr) are the Brick's, and
 * reach only parts of the left ring. frame_hex addresses each of the 23 LEDs
 * one by one, in this order (as found on a Brick Pro under MustardOS):
 *   1-8 the top bar, left to right;  9 F2;  10 F1
 *   11-14 the left ring, 15-18 the right ring (each upper left, lower left,
 *   lower right, upper right);  19 L1, 20 L2, 21 R2, 22 R1
 * so each stick gets its own ring. The named effects are off while the frame
 * shows (effect_enable 0) and started again at the end. JD_LED_FRAME=0 keeps
 * the named lights instead. */
#define NFRAME 23
#define RING_L 10 /* the left ring's first LED, counted from 0 */
#define RING_R 14
#define BAR 0 /* the top bar's first LED */
static int frame_mode;
static uint32_t frame_bar[8];
static uint32_t frame_shown[NFRAME];
static int frame_any;
static const char *const other_names[4] = {"f1", "f2", "m", "lr"};
static char kept_other[4][32];

static char led_dir[256];
static int leds_on, led_effect = 4;
static const char *const led_names[2] = {"l", "r"};
static char kept_rgb[2][32], kept_effect[2][32], kept_cycles[2][32], kept_scale[32], kept_enable[32];
static int set_scale; /* the brightness written, or 0 if left alone */
static uint32_t shown[2];
static int shown_any;

static void led_path(char *b, int n, const char *what, int k)
{
    snprintf(b, n, "%s/%s_%s", led_dir, what, led_names[k]);
}

static int put(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f)
        return 0;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

static void get(const char *path, char *b, int n)
{
    b[0] = 0;
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    if (!fgets(b, n, f))
        b[0] = 0;
    fclose(f);
    b[strcspn(b, "\r\n")] = 0;
}

static void led_file(char *b, int n, const char *what)
{
    snprintf(b, n, "%s/%s", led_dir, what);
}

int leds_open(const char *dir)
{
    snprintf(led_dir, sizeof led_dir, "%s", dir && *dir ? dir : "/sys/class/led_anim");
    const char *e = getenv("JD_LED_EFFECT");
    if (e && *e)
        led_effect = atoi(e);
    leds_on = shown_any = set_scale = frame_mode = frame_any = 0;
    for (int k = 0; k < 2; k++) {
        char p[300];
        led_path(p, sizeof p, "effect_rgb_hex", k);
        if (access(p, W_OK) != 0) {
            jdlog("lights: no stick lights in %s (%s)", led_dir, p);
            return 0;
        }
        get(p, kept_rgb[k], sizeof kept_rgb[k]);
        led_path(p, sizeof p, "effect", k);
        get(p, kept_effect[k], sizeof kept_effect[k]);
        led_path(p, sizeof p, "effect_cycles", k);
        get(p, kept_cycles[k], sizeof kept_cycles[k]);
        if (access(p, W_OK) == 0)
            put(p, "-1\n");
    }
    char p[300];
    led_file(p, sizeof p, "effect_enable");
    get(p, kept_enable, sizeof kept_enable);
#ifdef JD_BRICKPRO
    const char *fm = getenv("JD_LED_FRAME");
    led_file(p, sizeof p, "frame_hex");
    frame_mode = access(p, W_OK) == 0 && !(fm && *fm == '0');
#else
    {
        const char *fm = getenv("JD_LED_FRAME"); /* the Brick has no sticks: only on request (tests) */
        led_file(p, sizeof p, "frame_hex");
        frame_mode = fm && *fm == '1' && access(p, W_OK) == 0;
    }
#endif
    for (int k = 0; k < 4; k++) { /* the other lights' effects, started again at the end */
        snprintf(p, sizeof p, "%s/effect_%s", led_dir, other_names[k]);
        get(p, kept_other[k], sizeof kept_other[k]);
    }
    led_file(p, sizeof p, "effect_enable");
    led_file(p, sizeof p, "max_scale");
    get(p, kept_scale, sizeof kept_scale);
    int scale = kept_scale[0] ? atoi(kept_scale) : -1, want = 0;
    const char *b = getenv("JD_LED_BRIGHTNESS");
    if (b && *b)
        want = atoi(b);
    else if (scale == 0)
        want = 60;
    want = want < 0 ? 0 : want > 60 ? 60 : want;
    if (want > 0 && want != scale && access(p, W_OK) == 0) {
        char v[16];
        snprintf(v, sizeof v, "%d\n", want);
        if (put(p, v))
            set_scale = want;
    }
    if (frame_mode)
        jdlog("lights: each stick's own ring, LED by LED (frame_hex; JD_LED_FRAME=0: the named lights)");
    jdlog("lights: the sticks' lights follow the sticks (%s; they showed %s / %s, effect %s / %s; brightness %s%s)",
          led_dir, kept_rgb[0][0] ? kept_rgb[0] : "?", kept_rgb[1][0] ? kept_rgb[1] : "?",
          kept_effect[0][0] ? kept_effect[0] : "?", kept_effect[1][0] ? kept_effect[1] : "?",
          kept_scale[0] ? kept_scale : "?", set_scale ? ", turned up while playing" : "");
    if (scale == 0 && !set_scale)
        jdlog("HINT lights: their brightness is 0 and could not be turned up, so they stay dark");
    else if (set_scale)
        jdlog("lights: brightness %d while playing (JD_LED_BRIGHTNESS), %s again at the end", set_scale,
              kept_scale[0] ? kept_scale : "?");
    leds_on = 1;
    return 1;
}

static void led_set(int k, uint32_t rgb, const char *effect)
{
    char p[300], v[32];
    led_path(p, sizeof p, "effect_rgb_hex", k);
    snprintf(v, sizeof v, "%06X \n", (unsigned)(rgb & 0xffffff));
    put(p, v);
    led_path(p, sizeof p, "effect", k);
    put(p, effect);
}

static void led_enable(const char *v)
{
    char p[300];
    led_file(p, sizeof p, "effect_enable");
    if (access(p, W_OK) == 0)
        put(p, v);
}

void leds_show(uint32_t left, uint32_t right)
{
    if (!leds_on)
        return;
    uint32_t c[2] = {left, right};
    char eff[16];
    snprintf(eff, sizeof eff, "%d\n", led_effect);
    int changed = 0;
    for (int k = 0; k < 2; k++)
        if (!shown_any || c[k] != shown[k]) {
            led_set(k, c[k], eff);
            shown[k] = c[k];
            changed = 1;
        }
    if (changed)
        led_enable("1\n");
    shown_any = 1;
}

void stick_ring(float x, float y, uint32_t rgb, uint32_t out[4])
{
    static const float cx[4] = {-1.f, -1.f, 1.f, 1.f}, cy[4] = {1.f, -1.f, -1.f, 1.f};
    for (int k = 0; k < 4; k++) {
        float t = x * cx[k] + y * cy[k]; /* pushed straight up: both upper corners full */
        t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
        out[k] = stick_led_colour(rgb, floorf(t * 32.f + 0.5f) / 32.f); /* 32 steps: smooth, few writes */
    }
}

void leds_set_bar(const uint32_t bar[8])
{
    for (int k = 0; k < 8; k++)
        frame_bar[k] = bar ? bar[k] : 0;
}

void level_bar(float l, float r, uint32_t out[8])
{
    /* each side's four LEDs span -42 .. 0 dB, a quarter each; the LED the
     * level is in shows as bright as far into it as the level is */
    static const uint32_t col[4] = {0x20d040, 0x20d040, 0xffb020, 0xff2010};
    float lv[2] = {l, r};
    for (int c = 0; c < 2; c++) {
        float db = lv[c] > 1e-6f ? 20.f * log10f(lv[c]) : -120.f;
        float lit = (db + 42.f) / 42.f * 4.f; /* LEDs lit, from the centre */
        for (int k = 0; k < 4; k++) {
            float t = lit - k;
            t = t < 0.f ? 0.f : t > 1.f ? 1.f : t;
            out[c ? 4 + k : 3 - k] = stick_led_colour(col[k], floorf(t * 8.f + 0.5f) / 8.f);
        }
    }
}

static uint32_t brightest(const uint32_t c[4])
{
    uint32_t best = 0;
    int bs = -1;
    for (int k = 0; k < 4; k++) {
        int s = (int)((c[k] >> 16) & 255) + (int)((c[k] >> 8) & 255) + (int)(c[k] & 255);
        if (s > bs)
            bs = s, best = c[k];
    }
    return best;
}

void leds_show_rings(const uint32_t left[4], const uint32_t right[4])
{
    if (!leds_on)
        return;
    if (!frame_mode) {
        leds_show(brightest(left), brightest(right));
        return;
    }
    uint32_t f[NFRAME] = {0};
    for (int k = 0; k < 4; k++)
        f[RING_L + k] = left[k], f[RING_R + k] = right[k];
    for (int k = 0; k < 8; k++)
        f[BAR + k] = frame_bar[k];
    if (frame_any && !memcmp(f, frame_shown, sizeof f))
        return;
    if (!frame_any)
        led_enable("0\n"); /* the named effects off: the frame shows */
    char v[NFRAME * 7 + 2], p[300];
    for (int i = 0; i < NFRAME; i++)
        snprintf(v + i * 7, 8, "%06X ", (unsigned)(f[i] & 0xffffff));
    strcat(v, "\n");
    led_file(p, sizeof p, "frame_hex");
    put(p, v);
    memcpy(frame_shown, f, sizeof f);
    frame_any = 1;
}

void leds_close(void)
{
    if (!leds_on)
        return;
    if (frame_mode && frame_any) { /* dark, then the named effects started again below */
        char v[NFRAME * 7 + 2], p[300];
        for (int i = 0; i < NFRAME; i++)
            memcpy(v + i * 7, "000000 ", 7);
        v[NFRAME * 7] = '\n', v[NFRAME * 7 + 1] = 0;
        led_file(p, sizeof p, "frame_hex");
        put(p, v);
        for (int k = 0; k < 4; k++)
            if (kept_other[k][0]) {
                char e[160];
                snprintf(p, sizeof p, "%s/effect_%s", led_dir, other_names[k]);
                snprintf(e, sizeof e, "%s\n", kept_other[k]);
                put(p, e); /* writing an effect starts it */
            }
    }
    for (int k = 0; k < 2; k++) {
        char eff[80];
        snprintf(eff, sizeof eff, "%s\n", kept_effect[k][0] ? kept_effect[k] : "0");
        led_set(k, kept_rgb[k][0] ? (uint32_t)strtoul(kept_rgb[k], NULL, 16) : 0, eff);
        char p[300];
        led_path(p, sizeof p, "effect_cycles", k);
        if (kept_cycles[k][0] && access(p, W_OK) == 0) {
            snprintf(eff, sizeof eff, "%s\n", kept_cycles[k]);
            put(p, eff);
        }
    }
    char p[300], v[48];
    if (set_scale) {
        led_file(p, sizeof p, "max_scale");
        snprintf(v, sizeof v, "%s\n", kept_scale[0] ? kept_scale : "0");
        put(p, v);
    }
    snprintf(v, sizeof v, "%s\n", kept_enable[0] ? kept_enable : "1");
    led_enable(v);
    leds_on = 0;
    jdlog("lights: given back");
}
