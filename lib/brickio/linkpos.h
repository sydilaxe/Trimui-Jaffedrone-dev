/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The clock link's position message, as the sydilaxe apps send it to the Bricks following them, about
 * 50 times a second (link.h):
 *
 *   F0 7D 'T' 'L' <heard: 5 bytes> <bpm: 3 bytes> <playing> F7          14 bytes
 *
 * heard: the 16th notes heard at the moment it was sent, x 65536, five 7-bit bytes, low first;
 * bpm: x 1000, three 7-bit bytes, low first; playing: 0 or 1. 7D is MIDI's id for non-commercial
 * use, which other gear passes over. The follower turns its own heard position toward it (a jump when
 * more than half a 16th off, else a slight change of speed), so two Bricks sound together within a
 * fraction of a millisecond, not the few that MIDI clock ticks alone give. */
#pragma once
#include <stdint.h>

#define LINKPOS_LEN 14
#define LINK_TRANSIT_S 0.0003 /* a bulk transfer through a hub and the follower's thread waking */

/* the message into out (at least 14 bytes): its length, or 0 */
int linkpos_encode(uint8_t *out, int max, double heard, double bpm, int playing);
/* a SysEx (with or without its F0 / F7): 1 if it is the link's position, and its fields */
int linkpos_decode(const uint8_t *msg, int n, double *heard, double *bpm, int *playing);
