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
 * TrimUI's lights driver (led_anim, as TrimUI OS and NextUI drive it): for
 * each light, effect_rgb_hex_<name> takes a colour as six hex digits and
 * effect_<name> an effect (4: a steady colour). The sticks' lights are "l"
 * and "r". JD_LED_EFFECT changes the effect number for another firmware. */
static char led_dir[256];
static int leds_on, led_effect = 4;
static const char *const led_names[2] = {"l", "r"};
static char kept_rgb[2][32], kept_effect[2][32];
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

int leds_open(const char *dir)
{
    snprintf(led_dir, sizeof led_dir, "%s", dir && *dir ? dir : "/sys/class/led_anim");
    const char *e = getenv("JD_LED_EFFECT");
    if (e && *e)
        led_effect = atoi(e);
    leds_on = shown_any = 0;
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
    }
    char ms[300], scale[32];
    snprintf(ms, sizeof ms, "%s/max_scale", led_dir);
    get(ms, scale, sizeof scale);
    jdlog("lights: the sticks' lights follow the sticks (%s; they showed %s / %s, effect %s / %s; brightness %s)",
          led_dir, kept_rgb[0][0] ? kept_rgb[0] : "?", kept_rgb[1][0] ? kept_rgb[1] : "?",
          kept_effect[0][0] ? kept_effect[0] : "?", kept_effect[1][0] ? kept_effect[1] : "?",
          scale[0] ? scale : "?");
    if (scale[0] == '0' && scale[1] == 0)
        jdlog("HINT lights: their brightness is 0 (the lights are off in the system settings), so they stay dark");
    leds_on = 1;
    return 1;
}

static void led_set(int k, uint32_t rgb, const char *effect)
{
    char p[300], v[32];
    led_path(p, sizeof p, "effect_rgb_hex", k);
    snprintf(v, sizeof v, "%06X\n", (unsigned)(rgb & 0xffffff));
    put(p, v);
    led_path(p, sizeof p, "effect", k);
    put(p, effect);
}

void leds_show(uint32_t left, uint32_t right)
{
    if (!leds_on)
        return;
    uint32_t c[2] = {left, right};
    char eff[16];
    snprintf(eff, sizeof eff, "%d\n", led_effect);
    for (int k = 0; k < 2; k++)
        if (!shown_any || c[k] != shown[k]) {
            led_set(k, c[k], eff);
            shown[k] = c[k];
        }
    shown_any = 1;
}

void leds_close(void)
{
    if (!leds_on)
        return;
    for (int k = 0; k < 2; k++) {
        char eff[80];
        snprintf(eff, sizeof eff, "%s\n", kept_effect[k][0] ? kept_effect[k] : "0");
        led_set(k, kept_rgb[k][0] ? (uint32_t)strtoul(kept_rgb[k], NULL, 16) : 0, eff);
    }
    leds_on = 0;
    jdlog("lights: given back");
}
