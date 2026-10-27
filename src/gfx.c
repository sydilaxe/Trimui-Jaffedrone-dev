#include "gfx.h"

#include <math.h>
#include <string.h>

#include "font_data.h"

static uint32_t *fb;

void gfx_target(uint32_t *p) { fb = p; }

static inline uint32_t blend(uint32_t d, uint32_t c, int a)
{
    uint32_t rb = ((c & 0xff00ff) * a + (d & 0xff00ff) * (255 - a)) >> 8;
    uint32_t g = ((c & 0x00ff00) * a + (d & 0x00ff00) * (255 - a)) >> 8;
    return (rb & 0xff00ff) | (g & 0x00ff00);
}

uint32_t gfx_mix(uint32_t a, uint32_t b, float t)
{
    int k = (int)(t * 255.f);
    k = k < 0 ? 0 : k > 255 ? 255 : k;
    return blend(a, b, k);
}

void gfx_clear(uint32_t c)
{
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++)
        fb[i] = c;
}

static int clip(int *x, int *y, int *w, int *h)
{
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > SCREEN_W)
        *w = SCREEN_W - *x;
    if (*y + *h > SCREEN_H)
        *h = SCREEN_H - *y;
    return *w > 0 && *h > 0;
}

void gfx_fill(int x, int y, int w, int h, uint32_t c)
{
    if (!clip(&x, &y, &w, &h))
        return;
    for (int j = y; j < y + h; j++) {
        uint32_t *row = fb + j * SCREEN_W + x;
        for (int i = 0; i < w; i++)
            row[i] = c;
    }
}

void gfx_fill_a(int x, int y, int w, int h, uint32_t c, int alpha)
{
    if (!clip(&x, &y, &w, &h))
        return;
    for (int j = y; j < y + h; j++) {
        uint32_t *row = fb + j * SCREEN_W + x;
        for (int i = 0; i < w; i++)
            row[i] = blend(row[i], c, alpha);
    }
}

void gfx_rect(int x, int y, int w, int h, int t, uint32_t c)
{
    gfx_fill(x, y, w, t, c);
    gfx_fill(x, y + h - t, w, t, c);
    gfx_fill(x, y + t, t, h - 2 * t, c);
    gfx_fill(x + w - t, y + t, t, h - 2 * t, c);
}

/* Anti-aliased disc / ring via per-pixel distance coverage. */
static void shape(float cx, float cy, float r_out, float r_in, uint32_t c)
{
    int x0 = (int)floorf(cx - r_out - 1), x1 = (int)ceilf(cx + r_out + 1);
    int y0 = (int)floorf(cy - r_out - 1), y1 = (int)ceilf(cy + r_out + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > SCREEN_W) x1 = SCREEN_W;
    if (y1 > SCREEN_H) y1 = SCREEN_H;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            float dx = x + .5f - cx, dy = y + .5f - cy;
            float d = sqrtf(dx * dx + dy * dy);
            float a = r_out + .5f - d;
            if (r_in > 0.f) {
                float b = d - (r_in - .5f);
                if (b < a)
                    a = b;
            }
            if (a <= 0.f)
                continue;
            int ai = a >= 1.f ? 255 : (int)(a * 255.f);
            uint32_t *p = fb + y * SCREEN_W + x;
            *p = blend(*p, c, ai);
        }
}

void gfx_disc(float cx, float cy, float r, uint32_t c) { shape(cx, cy, r, 0.f, c); }
void gfx_ring(float cx, float cy, float r, float t, uint32_t c) { shape(cx, cy, r + t / 2, r - t / 2, c); }

int gfx_font_h(int font) { return font == FONT_LARGE ? FONT_LARGE_H : FONT_SMALL_H; }

int gfx_text_w(const char *s, int font)
{
    return (int)strlen(s) * (font == FONT_LARGE ? FONT_LARGE_W : FONT_SMALL_W);
}

int gfx_text(int x, int y, const char *s, uint32_t c, int font)
{
    int gw = font == FONT_LARGE ? FONT_LARGE_W : FONT_SMALL_W;
    int gh = font == FONT_LARGE ? FONT_LARGE_H : FONT_SMALL_H;
    for (; *s; s++, x += gw) {
        int ch = (unsigned char)*s;
        if (ch < 32 || ch > 126)
            ch = '?';
        const uint8_t *g = font == FONT_LARGE ? font_large_data[ch - 32] : font_small_data[ch - 32];
        for (int j = 0; j < gh; j++) {
            int py = y + j;
            if (py < 0 || py >= SCREEN_H)
                continue;
            for (int i = 0; i < gw; i++) {
                int px = x + i, a = g[j * gw + i];
                if (a && px >= 0 && px < SCREEN_W) {
                    uint32_t *p = fb + py * SCREEN_W + px;
                    *p = blend(*p, c, a);
                }
            }
        }
    }
    return x;
}

/* flat-filled triangle, scanline, for the angular panels */
void gfx_tri(float x0, float y0, float x1, float y1, float x2, float y2, uint32_t c)
{
    float ymin = y0 < y1 ? (y0 < y2 ? y0 : y2) : (y1 < y2 ? y1 : y2);
    float ymax = y0 > y1 ? (y0 > y2 ? y0 : y2) : (y1 > y2 ? y1 : y2);
    int ya = (int)ymin, yb = (int)ymax;
    if (ya < 0)
        ya = 0;
    if (yb >= SCREEN_H)
        yb = SCREEN_H - 1;
    for (int y = ya; y <= yb; y++) {
        float yc = y + 0.5f, xs[3];
        int n = 0;
        float px[3] = {x0, x1, x2}, py[3] = {y0, y1, y2};
        for (int e = 0; e < 3; e++) {
            float ax = px[e], ay = py[e], bx = px[(e + 1) % 3], by = py[(e + 1) % 3];
            if ((yc >= ay && yc < by) || (yc >= by && yc < ay))
                xs[n++] = ax + (yc - ay) * (bx - ax) / (by - ay);
        }
        if (n < 2)
            continue;
        float xa = xs[0] < xs[1] ? xs[0] : xs[1], xb = xs[0] < xs[1] ? xs[1] : xs[0];
        gfx_fill((int)xa, y, (int)(xb - xa) + 1, 1, c);
    }
}

void gfx_line(float x0, float y0, float x1, float y1, float t, uint32_t c)
{
    float r = t * 0.5f, dx = x1 - x0, dy = y1 - y0, ll = dx * dx + dy * dy;
    int bx0 = (int)floorf(fminf(x0, x1) - r - 1), bx1 = (int)ceilf(fmaxf(x0, x1) + r + 1);
    int by0 = (int)floorf(fminf(y0, y1) - r - 1), by1 = (int)ceilf(fmaxf(y0, y1) + r + 1);
    if (bx0 < 0) bx0 = 0;
    if (by0 < 0) by0 = 0;
    if (bx1 > SCREEN_W) bx1 = SCREEN_W;
    if (by1 > SCREEN_H) by1 = SCREEN_H;
    for (int y = by0; y < by1; y++)
        for (int x = bx0; x < bx1; x++) {
            /* coverage from the distance to the segment: a capsule */
            float px = x + .5f - x0, py = y + .5f - y0;
            float u = ll > 0.f ? (px * dx + py * dy) / ll : 0.f;
            u = u < 0.f ? 0.f : u > 1.f ? 1.f : u;
            float ex = px - u * dx, ey = py - u * dy;
            float a = r + .5f - sqrtf(ex * ex + ey * ey);
            if (a <= 0.f)
                continue;
            uint32_t *q = fb + y * SCREEN_W + x;
            int ai = a >= 1.f ? 255 : (int)(a * 255.f);
            *q = blend(*q, c, ai);
        }
}

static int poly_alpha = 255;
static void poly_fill(const float *xy, int n, uint32_t c, float period, float x0)
{
    float ymin = 1e9f, ymax = -1e9f;
    for (int i = 0; i < n; i++)
        ymin = fminf(ymin, xy[2 * i + 1]), ymax = fmaxf(ymax, xy[2 * i + 1]);
    int y0 = (int)ceilf(ymin - .5f), y1 = (int)floorf(ymax - .5f);
    if (y0 < 0) y0 = 0;
    if (y1 >= SCREEN_H) y1 = SCREEN_H - 1;
    for (int y = y0; y <= y1; y++) {
        float yc = y + .5f, xs[64];
        int k = 0;
        for (int i = 0, j = n - 1; i < n; j = i++) {
            float ya = xy[2 * j + 1], yb = xy[2 * i + 1];
            if ((ya <= yc) != (yb <= yc) && k < 64)
                xs[k++] = xy[2 * j] + (yc - ya) / (yb - ya) * (xy[2 * i] - xy[2 * j]);
        }
        for (int a = 1; a < k; a++) /* few crossings: insertion sort */
            for (int b = a; b > 0 && xs[b - 1] > xs[b]; b--) {
                float tmp = xs[b];
                xs[b] = xs[b - 1];
                xs[b - 1] = tmp;
            }
        for (int a = 0; a + 1 < k; a += 2) {
            int xa = (int)ceilf(xs[a] - .5f), xb = (int)floorf(xs[a + 1] - .5f);
            if (period <= 0.f) {
                if (xb >= xa && poly_alpha >= 255)
                    gfx_fill(xa, y, xb - xa + 1, 1, c);
                else if (xb >= xa)
                    gfx_fill_a(xa, y, xb - xa + 1, 1, c, poly_alpha);
                continue;
            }
            /* vertical grooves, one pixel wide, every period from x0 */
            float g = x0 + ceilf((xa - x0) / period) * period;
            for (; g <= xb; g += period)
                gfx_fill((int)g, y, 1, 1, c);
        }
    }
}

void gfx_poly(const float *xy, int n, uint32_t c) { poly_fill(xy, n, c, 0.f, 0.f); }
void gfx_poly_grooves(const float *xy, int n, uint32_t c, float period, float x0) { poly_fill(xy, n, c, period, x0); }
void gfx_poly_a(const float *xy, int n, uint32_t c, int alpha)
{
    poly_alpha = alpha;
    poly_fill(xy, n, c, 0.f, 0.f);
    poly_alpha = 255;
}

/* add light: each channel of c is added, saturating at full. All three
 * channels at once: halve both (dropping their lowest bit), add, and where
 * a channel overflows its top bit fill it with ones */
void gfx_add(int x, int y, int w, int h, uint32_t c)
{
    if (!clip(&x, &y, &w, &h))
        return;
    uint32_t ch = (c & 0xfefefe) >> 1;
    for (int j = y; j < y + h; j++) {
        uint32_t *row = fb + j * SCREEN_W + x;
        for (int i = 0; i < w; i++) {
            uint32_t t = ((row[i] & 0xfefefe) >> 1) + ch;
            uint32_t m = t & 0x808080;
            row[i] = ((t << 1) & 0xfefefe) | ((m << 1) - (m >> 7));
        }
    }
}
