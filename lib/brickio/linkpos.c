/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The clock link's position message: see linkpos.h. The same bytes as the apps' midi_link_pos. */
#include "linkpos.h"

#include <math.h>

int linkpos_encode(uint8_t *out, int max, double heard, double bpm, int playing)
{
    if (max < LINKPOS_LEN) return 0;
    uint64_t p = heard > 0.0 ? (uint64_t)llround(heard * 65536.0) : 0;
    uint32_t b = (uint32_t)lround(bpm * 1000.0);
    int n = 0;
    out[n++] = 0xf0, out[n++] = 0x7d, out[n++] = 'T', out[n++] = 'L';
    for (int k = 0; k < 5; k++) out[n++] = (uint8_t)(p >> (7 * k) & 127);
    for (int k = 0; k < 3; k++) out[n++] = (uint8_t)(b >> (7 * k) & 127);
    out[n++] = playing ? 1 : 0;
    out[n++] = 0xf7;
    return n;
}

int linkpos_decode(const uint8_t *m, int n, double *heard, double *bpm, int *playing)
{
    if (n > 0 && m[0] == 0xf0) m++, n--;
    if (n > 0 && m[n - 1] == 0xf7) n--;
    if (n != 12 || m[0] != 0x7d || m[1] != 'T' || m[2] != 'L') return 0;
    uint64_t p = 0;
    uint32_t b = 0;
    for (int k = 0; k < 5; k++) p |= (uint64_t)m[3 + k] << (7 * k);
    for (int k = 0; k < 3; k++) b |= (uint32_t)m[8 + k] << (7 * k);
    if (heard) *heard = p / 65536.0;
    if (bpm) *bpm = b / 1000.0;
    if (playing) *playing = m[11] & 1;
    return 1;
}
