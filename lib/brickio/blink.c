/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Brick link's frames (see blink.h). */
#include "blink.h"
#include <string.h>

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8); }
static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }

int blink_frame(uint8_t *out, int max, uint8_t type, uint8_t port, uint16_t seq, const void *payload, int len)
{
    if (len < 0 || len > BLINK_MAX_PAYLOAD || BLINK_HDR + len > max)
        return 0;
    out[0] = 'B', out[1] = type, out[2] = port, out[3] = 0;
    put16(out + 4, (unsigned)len), put16(out + 6, seq);
    if (len)
        memcpy(out + BLINK_HDR, payload, (size_t)len);
    return BLINK_HDR + len;
}

int blink_pad(uint8_t *buf, int max, int n)
{
    if (n <= 0 || n % 64)
        return n;
    int k = blink_frame(buf + n, max - n, BLINK_NOP, 0, 0, NULL, 0);
    return n + k;
}

void blink_rx_init(BlinkRx *rx) { rx->n = rx->pos = 0, rx->bad = 0; }

int blink_rx_feed(BlinkRx *rx, const uint8_t *b, int n)
{
    if (rx->pos) { /* what was read goes */
        memmove(rx->buf, rx->buf + rx->pos, (size_t)(rx->n - rx->pos));
        rx->n -= rx->pos, rx->pos = 0;
    }
    if (n < 0 || n > (int)sizeof rx->buf - rx->n) {
        rx->bad += (unsigned)rx->n;
        rx->n = 0;
        return 0;
    }
    memcpy(rx->buf + rx->n, b, (size_t)n);
    rx->n += n;
    return 1;
}

int blink_rx_next(BlinkRx *rx, BlinkFrame *f)
{
    for (;;) {
        int left = rx->n - rx->pos;
        if (left < BLINK_HDR)
            return 0;
        const uint8_t *p = rx->buf + rx->pos;
        unsigned len = get16(p + 4);
        if (p[0] != 'B' || p[3] != 0 || len > BLINK_MAX_PAYLOAD) { /* not a frame: look for the next */
            rx->pos++, rx->bad++;
            continue;
        }
        if (left < BLINK_HDR + (int)len)
            return 0;
        f->type = p[1], f->port = p[2], f->flags = p[3];
        f->len = (uint16_t)len, f->seq = (uint16_t)get16(p + 6);
        f->payload = p + BLINK_HDR;
        rx->pos += BLINK_HDR + (int)len;
        return 1;
    }
}

/* ------------------------------------------------------------ HELLO */

#define HELLO_HEAD 31 /* version, app 2, ver 3, name 24, nports */
#define HELLO_PORT 19 /* kind, id, channels, name 16 */

int blink_hello_pack(uint8_t *out, int max, const BlinkHello *h)
{
    int np = h->nports < 0 ? 0 : h->nports > BLINK_MAX_PORTS ? BLINK_MAX_PORTS : h->nports;
    int len = HELLO_HEAD + np * HELLO_PORT;
    if (len > max)
        return 0;
    memset(out, 0, (size_t)len);
    out[0] = BLINK_VERSION;
    memcpy(out + 1, h->app, strnlen(h->app, 2));
    memcpy(out + 3, h->ver, 3);
    memcpy(out + 6, h->name, strnlen(h->name, 24));
    out[30] = (uint8_t)np;
    for (int i = 0; i < np; i++) {
        uint8_t *q = out + HELLO_HEAD + i * HELLO_PORT;
        q[0] = h->ports[i].kind, q[1] = h->ports[i].id, q[2] = h->ports[i].channels;
        memcpy(q + 3, h->ports[i].name, strnlen(h->ports[i].name, 16));
    }
    return len;
}

int blink_hello_read(const BlinkFrame *f, BlinkHello *h)
{
    memset(h, 0, sizeof *h);
    if (f->type != BLINK_HELLO || f->len < HELLO_HEAD)
        return 0;
    const uint8_t *p = f->payload;
    int np = p[30];
    if (np > BLINK_MAX_PORTS || f->len < HELLO_HEAD + np * HELLO_PORT)
        return 0;
    h->version = p[0];
    memcpy(h->app, p + 1, 2);
    memcpy(h->ver, p + 3, 3);
    memcpy(h->name, p + 6, 24);
    h->nports = np;
    for (int i = 0; i < np; i++) {
        const uint8_t *q = p + HELLO_HEAD + i * HELLO_PORT;
        h->ports[i].kind = q[0], h->ports[i].id = q[1], h->ports[i].channels = q[2];
        memcpy(h->ports[i].name, q + 3, 16);
    }
    return 1;
}

/* ------------------------------------------------------------ AUDIO */

#define AUDIO_HEAD 16

int blink_audio_pack(uint8_t *out, int max, uint32_t rate, int frames, int channels, double heard, const float *samples)
{
    if (frames < 0 || frames > 0xffff || channels < 1 || channels > 255)
        return 0;
    long len = AUDIO_HEAD + (long)frames * channels * 4;
    if (len > max || len > BLINK_MAX_PAYLOAD)
        return 0;
    for (int i = 0; i < 4; i++)
        out[i] = (uint8_t)(rate >> (8 * i));
    put16(out + 4, (unsigned)frames);
    out[6] = (uint8_t)channels, out[7] = 0;
    memcpy(out + 8, &heard, 8);
    memcpy(out + AUDIO_HEAD, samples, (size_t)frames * (size_t)channels * 4);
    return (int)len;
}

int blink_audio_read(const BlinkFrame *f, BlinkAudio *a)
{
    if (f->type != BLINK_AUDIO || f->len < AUDIO_HEAD)
        return 0;
    const uint8_t *p = f->payload;
    a->rate = p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
    a->frames = (uint16_t)get16(p + 4);
    a->channels = p[6], a->format = p[7];
    memcpy(&a->heard, p + 8, 8);
    a->samples = p + AUDIO_HEAD;
    return a->format == 0 && a->channels >= 1 &&
           f->len == AUDIO_HEAD + (long)a->frames * a->channels * 4;
}

void blink_samples(const BlinkAudio *a, float *dst)
{
    memcpy(dst, a->samples, (size_t)a->frames * a->channels * 4);
}
