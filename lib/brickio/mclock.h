/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The musical clock: MIDI clock and the USB clock link, in and out, on one timeline, the sound as it
 * is heard. From the sydilaxe apps' midi.c (Columbia Street, HI5, Jaffedrone, RadioScan), the same
 * logic, talking to the app through a ClockHost of callbacks instead of its globals.
 *
 *   in:   MIDI clock (F8) sets the tempo from a straight line through the last four beats of ticks
 *         (their arrival times: USB's jitter and the thread waking average out), and holds the phase
 *         of what is heard to the ticks by trimming the app's speed (a jump only when far off).
 *         START / CONTINUE / STOP start, carry on and stop the app (unless set to the clock only).
 *         A leading Brick's link position (linkpos.h) wins over the ticks: its exact tempo and
 *         phase, within a fraction of a millisecond.
 *   out:  the clock ticks, START and STOP, stepped from what is heard (the audio clock, aclock.h,
 *         or the position less the output's latency), so the ticks fall with the sound, not ahead
 *         of it; and the link position for following Bricks, every 20 ms.
 *
 * Two differences from the apps' code, both for gear other than the apps themselves (tests/test_mclock.c):
 *   - the first tick after START is the downbeat, as MIDI 1.0 has it. The apps counted it as the
 *     16th / 6 after, which came right only because they skipped the first tick when no tempo was
 *     known yet (the apps' own clock starts with START); from a drum machine whose clock runs all the
 *     time, they ran a tick (~21 ms at 120 BPM) ahead.
 *   - a tempo change is taken in one beat, not four: the last beat's ticks are fitted too, and when
 *     they disagree with the four beats' by more than 1 % the fit starts again from that beat (the
 *     phase has half as far to catch up after a jump in tempo).
 *
 * Everything takes the time (CLOCK_MONOTONIC microseconds) as an argument and does no I/O: the
 * MIDI thread calls it with each byte's arrival time and sends what it returns; the tests drive it
 * with made-up times. The ClockHost's functions are called from that thread: they must be safe to
 * call while the audio runs (atomics, or the app's lock). */
#pragma once
#include <stdint.h>

/* who leads: AUTO follows a clock coming in and sends one otherwise; FOLLOW only follows; SEND only
 * sends; OFF neither */
enum { MCLOCK_AUTO, MCLOCK_FOLLOW, MCLOCK_SEND, MCLOCK_OFF };

typedef struct {
    float (*bpm)(void);
    void (*set_bpm)(float bpm);
    double (*position)(void);          /* where the app's sound is being made, in 16th notes */
    void (*set_position)(double steps);
    void (*set_rate)(double rate);     /* the app's speed trim, 1.0 nominal (the phase lock's handle) */
    int (*playing)(void);
    void (*set_playing)(int on);
} ClockHost;

void mclock_init(const ClockHost *host);
void mclock_reset(void);               /* forget what came in (a new song, say) */

/* settings: any thread, any time */
void mclock_set_mode(int mode);        /* MCLOCK_*, AUTO by default */
void mclock_set_transport(int on);     /* 0: the clock only, START / STOP ignored (and the link lines
                                        * up the bar, not the song); 1 by default */
void mclock_set_relay(int on);         /* 1: while following a clock (or a leading Brick), send one on too, stepped
                                        * from this app's heard sound (locked to the one coming in), with START and
                                        * STOP: gear further down lines up with what this app plays. 0 by default
                                        * (only the link position is passed on, as before). mclockio leaves out the
                                        * port the clock comes from */
void mclock_set_ratio(double in, double out); /* the clock's tempo to the app's: in 0.5 follows a clock at
                                        * half its tempo (half time), out 2 sends one at double the app's
                                        * (1/8..8; 1 by default). MIDI clock only: a leading Brick's link is
                                        * followed at its own tempo */
void mclock_set_start_out(int on);     /* 0: the clock goes out without START and STOP (1 by default) */
void mclock_set_latency(float seconds);/* from the app's position to the sound heard, when there is
                                        * no audio clock (aclock) running: the output's real queue */

/* in: each real-time byte (F8 FA FB FC) and each link position, with its arrival time */
void mclock_realtime(uint8_t b, uint64_t t_us);
void mclock_link_position(uint64_t t_us, double lead, double bpm, int playing);
/* in: song position (F2), in 16th notes: the next tick marks that 16th (a sequencer CONTINUEing from the
 * middle of a song sends it first); while playing, the app moves there now. Ignored set to the clock only */
void mclock_song_position(int sixteenths);

/* out: the bytes to send to the MIDI ports now (call every millisecond or so from the MIDI thread),
 * and the link position for following Bricks (every 20 ms; 0 bytes otherwise) */
int mclock_step(uint64_t t_us, uint8_t *out, int max);
int mclock_link_step(uint64_t t_us, uint8_t *out, int max);

/* the outside clock: let go of it when it has gone quiet for a second (call with mclock_step) */
void mclock_watch(uint64_t t_us);
int mclock_following(void);            /* an outside clock (or a leading Brick) leads */
int mclock_sending(void);              /* this one's clock is going out */
