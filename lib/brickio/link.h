/* The clock link between Bricks over USB, with no MIDI interface between
 * them: the leader's top (host) port goes to a hub, and each follower's
 * bottom port to the same hub. A follower presents its bottom port as a
 * "TrimUI clock link" device (USB gadget mode through configfs and
 * FunctionFS: a vendor interface with one bulk OUT endpoint, as TrimUI's
 * kernel has no USB MIDI gadget), and what arrives on it is MIDI (clock,
 * START, STOP, song position), fed to midi.c as if from a MIDI port. The
 * leader finds the followers on its USB bus (link_find) and sends them what
 * it sends its MIDI ports (midi.c), through usbfs. So the wiring decides who
 * leads, and any number of Bricks can follow one.
 *
 * Wire-compatible with Conduit, Columbia Street, HI5, Jaffedrone and RadioScan (the descriptors and the
 * gadget are the same bytes; tests/test_link.c checks them). <APP>_LINK=0 in launch.sh leaves the bottom
 * port alone. */
#pragma once
#include <stdint.h>

#define LINK_VID 0x1d6b /* the Linux Foundation's composite gadget id */
#define LINK_PID 0x0104
#define LINK_SUBCLASS 0x42 /* 'B' */
#define LINK_PROTOCOL 0x4c /* 'L' */

/* the follower: set up the bottom port (1) or not (0: no device controller,
 * no configfs, the port in use, not root, or <app>_LINK=0); and give it back.
 * What arrives (MIDI bytes) goes to on_bytes, on the link's thread, with its arrival time
 * (CLOCK_MONOTONIC, microseconds); notice (or NULL) gets a line to show, e.g. when a leader starts */
int link_follow_start(const char *app, void (*on_bytes)(const uint8_t *b, int n, uint64_t t_us, void *ud), void *ud,
                      void (*notice)(const char *msg));
void link_follow_stop(void);
/* before link_follow_start: the follower's reader at real-time priority (SCHED_FIFO prio; 0, the default:
 * as other threads), so the leader's bytes are stamped as they arrive (see midi_set_priority) */
void link_follow_priority(int prio);
/* 0 not set up, 1 waiting for a leader, 2 a leader sending (in the last 2 s) */
int link_follow_state(void);

/* the leader: linked Bricks under a sysfs USB devices folder
 * (/sys/bus/usb/devices), at most max */
typedef struct {
    int bus, dev, ep; /* the usbfs device and its bulk OUT endpoint address */
    char path[300];   /* the sysfs folder, to tell when it goes */
} LinkDev;
int link_find(const char *sysfs, LinkDev *out, int max);
/* open one (claims its interface): a file descriptor, or -1 */
int link_open(const LinkDev *d);
/* send to it: 1 sent, 0 not this time (busy), -1 gone (close it) */
int link_send(int fd, int ep, const uint8_t *b, int n);

/* the Brick link's data channel (blink.h, blinkio.h): before link_follow_start, 1 asks for a second
 * interface on the bottom port (bulk, each way) beside the clock link, whose interface 0 stays as it is.
 * Off by default. If the port won't take both, the clock link is set up alone, as before */
void link_follow_data(int on);
/* after link_follow_start: the data channel's endpoint files (out_fd: what the leader sends, to read;
 * in_fd: to write), 1 if it is up. The caller closes them */
int link_data_open(int *out_fd, int *in_fd);

/* what the follower writes to FunctionFS, for the tests: the length */
int link_descriptors(uint8_t *buf, int max);
int link_strings(uint8_t *buf, int max);
int link_data_descriptors(uint8_t *buf, int max);
int link_data_strings(uint8_t *buf, int max);
