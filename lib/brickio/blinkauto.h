/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The Brick link's data channel, managed (blinkio.h is the session, docs/brick-audio.md the design): what
 * an app needs to send its sound to other Bricks, or take theirs, without minding which are plugged in.
 *
 *   leader (the Brick whose top port has the hub): a thread finds the receiving Bricks on the hub every
 *          second (bl_find), opens a session with each (bl_usb_leader) and drops one that has gone
 *   follower (a receiving Brick, its bottom port on the leader's hub): after link_follow_data(1) and
 *          link_follow_start, a thread opens the session on the data channel, and again if it goes
 *   the audio thread: bla_send sends a block to every session (lock-free, never blocks), bla_pull takes
 *          the first peer's audio. Sessions come and go under it safely
 *
 * One manager per app (it is either side, not both). The data channel is off in link.c by default, so an
 * app that takes brickio and calls none of this behaves as before. */
#pragma once
#include <stdint.h>

#include "blinkio.h"

#define BLA_MAX 4 /* sessions at once (Bricks on the leader's hub) */

/* the leader: the session's config (each session gets a copy), and where to look (NULL:
 * /sys/bus/usb/devices). 0 on success */
int bla_lead_start(const BlConfig *c, const char *sysfs);
/* the follower: after link_follow_data(1) and link_follow_start. 0 on success */
int bla_follow_start(const BlConfig *c);
/* the sessions' config without a thread (a test, or an app that brings its own transports); the starts
 * above call it */
void bla_init(const BlConfig *c);
/* a session on any transport (the threads use it; tests give a socket pair): 1 added */
int bla_add(BlTransport t);
void bla_stop(void);

/* the audio thread: stereo frames out on a port to every session (heard: the music's position of the
 * block's first frame, in 16ths, or -1); stereo frames from the first session with a peer's port into dst,
 * 1 sound, 0 silence (*heard as bl_pull's) */
void bla_send(int port, const float *stereo, int frames, double heard);
int bla_pull(int port, float *dst, int frames, double *heard);

/* any thread: sessions open, and with the other side heard from; the first peer's HELLO */
int bla_sessions(void);
int bla_peers(BlinkHello *first);
