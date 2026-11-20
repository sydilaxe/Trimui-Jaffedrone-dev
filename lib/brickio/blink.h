/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Brick link's data channel: audio and MIDI between Bricks, both ways, over the receiving Brick's
 * bottom USB port, beside the clock link (link.h), which stays as it is. See docs/brick-audio.md.
 *
 * The receiver's gadget gets a second interface (vendor class, subclass 'B', protocol 'D') with a bulk
 * endpoint each way: the Brick's kernel has no USB audio gadget, and its controller won't run an
 * isochronous endpoint each way. What goes over either is a stream of frames, several to a transfer
 * (a frame may also be split across two):
 *
 *   0  'B'                 magic
 *   1  type                BLINK_* below. Others are passed over, so a newer side can add types
 *   2  port                the sending side's port it comes from (its HELLO names it)
 *   3  flags               0 for now
 *   4  u16 length          of the payload, little-endian
 *   6  u16 sequence        counts per type and port: a gap is a lost frame
 *   8  payload
 *
 *   NOP     nothing (pads a transfer that would end on a packet boundary: see blink_pad)
 *   HELLO   who this side is and what it offers: u8 version (2), app code (2 chars), its version
 *           (3 bytes), name (24 bytes, NUL-padded), u8 port count, then each port: u8 kind
 *           (BLINK_PORT_*), u8 id, u8 channels, name (16 bytes). Sent when the link comes up and every
 *           half second after (so each side knows the other is there); the other side lists the ports
 *   AUDIO   u32 rate, u16 frames, u8 channels, u8 format (0: float32), f64 the sender's heard position
 *           (16ths, for tempo-synced effects; -1 when none), then frames x channels samples, interleaved
 *   MIDI    MIDI bytes as they came (any messages, clock and SysEx included), from that port
 *   PING    u64 the sender's time (us); answered by PONG with the same 8 bytes: the link's round trip
 *
 * Everything here packs and reads frames; nothing does I/O. All little-endian (the Brick is). */
#pragma once
#include <stdint.h>

#define BLINK_VERSION 2
#define BLINK_HDR 8
#define BLINK_MAX_PAYLOAD 12288 /* 1024 stereo float frames and their header, with room */
#define BLINK_MAX_TRANSFER 16384
#define BLINK_SUBCLASS 0x42 /* 'B' */
#define BLINK_PROTOCOL 0x44 /* 'D': data (the clock link's is 'L') */

enum { BLINK_NOP = 0, BLINK_HELLO = 1, BLINK_AUDIO = 2, BLINK_MIDI = 3, BLINK_PING = 4, BLINK_PONG = 5 };
enum { BLINK_PORT_AUDIO_IN = 1, BLINK_PORT_AUDIO_OUT = 2, BLINK_PORT_MIDI_IN = 3, BLINK_PORT_MIDI_OUT = 4 };
#define BLINK_MAX_PORTS 16

typedef struct {
    uint8_t type, port, flags;
    uint16_t len, seq;
    const uint8_t *payload; /* into the buffer it was read from */
} BlinkFrame;

typedef struct {
    uint8_t kind, id, channels;
    char name[17];
} BlinkPort;

typedef struct {
    uint8_t version, ver[3];
    char app[3], name[25];
    int nports;
    BlinkPort ports[BLINK_MAX_PORTS];
} BlinkHello;

typedef struct {
    uint32_t rate;
    uint16_t frames;
    uint8_t channels, format;
    double heard;            /* the sender's heard position, 16ths (-1: none) */
    const uint8_t *samples;  /* frames x channels float32, interleaved, maybe unaligned: blink_samples */
} BlinkAudio;

/* a frame into out: its length (BLINK_HDR + len), or 0 if it doesn't fit */
int blink_frame(uint8_t *out, int max, uint8_t type, uint8_t port, uint16_t seq, const void *payload, int len);
/* a transfer of n bytes ending on a packet boundary (a multiple of 64) would leave the other side's
 * read waiting for more: then a NOP goes on the end. Returns the new length (n or n + BLINK_HDR) */
int blink_pad(uint8_t *buf, int max, int n);

/* reading: bytes in as they come (whole transfers or not), whole frames out */
typedef struct {
    uint8_t buf[2 * BLINK_MAX_TRANSFER];
    int n, pos;
    unsigned bad; /* bytes passed over to find the next frame (a damaged or foreign stream) */
} BlinkRx;
void blink_rx_init(BlinkRx *rx);
/* add bytes: 0 if they didn't fit (the frames before them weren't read: everything is dropped) */
int blink_rx_feed(BlinkRx *rx, const uint8_t *b, int n);
/* the next whole frame: 1 and *f (valid until the next feed), or 0 for none yet */
int blink_rx_next(BlinkRx *rx, BlinkFrame *f);

int blink_hello_pack(uint8_t *out, int max, const BlinkHello *h); /* the payload: its length, or 0 */
int blink_hello_read(const BlinkFrame *f, BlinkHello *h);         /* 1 if it was a whole hello */
/* an AUDIO payload from frames x channels interleaved floats: its length, or 0 */
int blink_audio_pack(uint8_t *out, int max, uint32_t rate, int frames, int channels, double heard, const float *samples);
int blink_audio_read(const BlinkFrame *f, BlinkAudio *a);         /* 1 if whole */
void blink_samples(const BlinkAudio *a, float *dst);              /* frames x channels into dst */
