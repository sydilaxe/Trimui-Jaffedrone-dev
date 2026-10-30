/* MIDI over USB: class-compliant keyboards, controllers, sequencers and
 * drum machines on the TrimUI's USB-C port (an OTG adapter or hub), read and
 * written through ALSA's raw MIDI ports (/dev/snd/midiC*D*), so nothing more
 * is needed on the device. Ports are found as they are plugged in.
 *
 * In (any channel):
 *   notes          NOTES=POLY (default): a key takes a voice and tunes it
 *                  to the key played, as a bend
 *                  on top of the song (the song itself is never changed);
 *                  it plucks as hard as the key is struck and bows while
 *                  held (a wind: tongues and blows; LATCH: drones until the
 *                  key is let go, a soft key swelling in with the bow).
 *                  The first key plays the voice selected on PLAY; each
 *                  key added while others are held takes the next free
 *                  voice round from the last one taken (a chord builds
 *                  across the voices, a run of keys rotates through them);
 *                  with every voice held, the key held longest gives its
 *                  voice up. Muted and listening (AURA) voices are skipped.
 *                  NOTES=CHANNELS: channels 1-5 play strings 1-5 the same
 *                  way, other channels the selected string.
 *   pitch bend     two semitones
 *   CC1, pressure  how hard the held strings are bowed
 *   CC64           sustain
 *   CC74           TONE, darker / brighter;  CC91 more / less reverb
 *   CC120, CC123   everything let go
 *   other CCs      the important settings (ui_midi_cc; see docs/MIDI.md)
 *   clock          the tempo follows it; START, CONTINUE, STOP and song
 *                  position start, stop and place the drone
 * Out, to every port: clock (24 a quarter note), START and STOP, while the
 * drone plays and no clock is coming in. The clock goes out as the sound is
 * heard, the render-ahead time behind the engine.
 *
 * Settings, in launch.sh: JD_MIDI=0 turns it off, JD_MIDI_NOTES=CHANNELS,
 * JD_MIDI_CLOCK=0 sends no clock, JD_MIDI_FOLLOW=0 ignores clock in. */
#pragma once
#include <stdint.h>

void midi_start(int core);   /* the thread (pinned to core, -1: anywhere) */
void midi_stop(void);
void midi_set_latency(float seconds); /* from the engine's position to the sound heard */
/* messages for the screen ("MIDI: <device>"), main thread */
int midi_take_notice(char *buf, int n);
/* the next CC that isn't one of the fixed ones above (for ui_midi_cc), main
 * thread; 0 when there's none */
int midi_take_cc(int *ch, int *cc, int *v);

/* the parser and its actions without the thread or devices, for the test
 * harness: bytes in, as if from a port, at a time in microseconds */
void midi_feed(const uint8_t *b, int n, uint64_t now_us);
void midi_reset(void);
int midi_voice_of(int ch, int note); /* the voice a held key plays, or -1 */
/* the clock out, stepped by hand: returns the bytes it would send now */
int midi_clock_step(uint64_t now_us, uint8_t *out, int max);
