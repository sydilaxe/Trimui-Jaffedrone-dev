/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Brick link's data channel in use: one session per Brick at the other end (blink.h has the
 * frames, docs/brick-audio.md the design).
 *
 *   - audio out: the app's audio thread hands its output to bl_send_audio (lock-free, never blocks);
 *     a sending thread packs what has come into frames and sends them at once, in one transfer
 *   - audio in: a receiving thread puts each port's audio into a ring of its own (aring.h: a small
 *     cushion, the two Bricks' clocks followed by resampling), and the app's audio thread takes it
 *     with bl_pull, as it takes a USB interface's input
 *   - MIDI both ways, by port, in the same stream
 *   - HELLO when the link comes up and every half second: each side lists the other's ports, and
 *     knows within a second and a half when it has gone
 *   - PING / PONG: the link's own round trip, for the log
 *
 * The transport is anything that moves bytes: the USB ones are below (the leader through usbfs, the
 * follower through the data channel beside the clock link); the tests use a socket pair. */
#pragma once
#include <stdint.h>

#include "blink.h"

#define BL_PORTS 4 /* audio ports each way, numbered 0-3 */

typedef struct {
    /* send n bytes (whole frames): 1 sent, 0 not now (drop and go on), -1 not connected now, -2 gone for good */
    int (*send)(void *ctx, const uint8_t *b, int n);
    /* receive up to max bytes: their count, 0 none yet, -1 not connected now, -2 gone for good. May block:
     * bl_stop interrupts it with a signal */
    int (*recv)(void *ctx, uint8_t *b, int max);
    void (*close)(void *ctx);
    void *ctx;
} BlTransport;

typedef struct {
    const char *app;    /* the app's two-letter code ("FX") */
    const char *name;   /* its name ("TrimFX") */
    uint8_t ver[3];     /* its version */
    int rate;           /* audio frames a second (0: 48000) */
    int nports;         /* what this side offers (shown by the other) */
    BlinkPort ports[BLINK_MAX_PORTS];
    /* MIDI from the other side, on the receiving thread, stamped on arrival (CLOCK_MONOTONIC, us) */
    void (*on_midi)(int port, const uint8_t *b, int n, uint64_t t_us, void *ud);
    /* the other side came (up 1, with its HELLO) or went (up 0), on the receiving thread */
    void (*on_peer)(const BlinkHello *h, int up, void *ud);
    void *ud;
} BlConfig;

typedef struct BlSession BlSession;

/* the session takes the transport (closes it in bl_stop). NULL if it couldn't start */
BlSession *bl_start(const BlConfig *c, BlTransport t);
void bl_stop(BlSession *s);
/* 1 while the other side is heard from; *h its HELLO (h may be NULL) */
int bl_peer(BlSession *s, BlinkHello *h);
/* 0 once the transport has gone for good (a Brick unplugged): stop the session */
int bl_alive(BlSession *s);

/* the audio thread: stereo frames out on a port (heard: the music's position in 16ths, or -1).
 * Never blocks; 0 if there was no room (the frames are dropped and counted) */
int bl_send_audio(BlSession *s, int port, const float *stereo, int frames, double heard);
/* the audio thread: stereo frames from the other side's port into dst; 1 sound, 0 silence (nothing
 * yet, or priming). *heard (may be NULL): the sender's heard position with the frames, or -1 */
int bl_pull(BlSession *s, int port, float *dst, int frames, double *heard);

/* any thread but the audio one */
void bl_send_midi(BlSession *s, int port, const uint8_t *b, int n);
void bl_ping(BlSession *s);

typedef struct {
    int peer;                      /* the other side heard from */
    unsigned tx_frames, tx_drops;  /* audio frames sent, dropped for want of room (all ports) */
    unsigned rx_frames, rx_lost;   /* audio frames received, frames lost (sequence gaps, all ports) */
    unsigned rx_bad;               /* bytes passed over (damage) */
    unsigned transfers_out, transfers_in, send_fails;
    float rtt_ms;                  /* the last PING's round trip (0: none yet) */
    /* the port asked for, as received: */
    unsigned underruns, skips, cushion; /* the ring's (frames) */
    int drift_ppm;
    int rate;                      /* its sender's rate (0: none yet) */
} BlStats;
void bl_stats(BlSession *s, int port, BlStats *st);

/* ------------------------------------------------------------ USB */

/* the follower (the receiving Brick): its data channel, set up beside the clock link
 * (link_follow_data(1) before link_follow_start). 1 and *t if it is up */
int bl_usb_follower(BlTransport *t);

/* the leader: Bricks with the data channel on its USB bus (a sysfs USB devices folder,
 * /sys/bus/usb/devices), at most max */
typedef struct {
    int bus, dev, iface;  /* the usbfs device and the data interface's number */
    int ep_out, ep_in;    /* its bulk endpoints' addresses */
    char path[300];       /* the sysfs folder */
} BlinkDev;
int bl_find(const char *sysfs, BlinkDev *out, int max);
/* open one (claims the data interface): 1 and *t */
int bl_usb_leader(const BlinkDev *d, BlTransport *t);
