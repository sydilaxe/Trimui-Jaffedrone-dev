/* MIDI over USB: class-compliant keyboards, controllers, sequencers and
 * drum machines on the TrimUI's USB-C port (an OTG adapter or hub), read and
 * written through ALSA's raw MIDI ports (/dev/snd/midiC*D*), so nothing more
 * is needed on the device. Ports are found as they are plugged in.
 *
 * In (any channel):
 *   notes          NOTES=POLY (default): the keyboard plays the voice
 *                  selected on PLAY, NKEY (four) notes at once: each key
 *                  sounds that string's voice at the key's pitch on a key
 *                  voice of its own (engine_key_voice), beside the strings,
 *                  so the drone and the song are untouched. It plucks as
 *                  hard as the key is struck and bows while held (a wind:
 *                  tongues and blows; LATCH: drones until the key is let
 *                  go, a soft key swelling in with the bow). A key goes to
 *                  a silent key voice, else the one let go longest ago;
 *                  with four held, the key held longest gives its voice up.
 *                  NOTES=CHANNELS: channels 1-5 play strings 1-5, other
 *                  channels the selected string, each key tuning its
 *                  string to itself as a bend on top of the song.
 *   pitch bend     two semitones
 *   CC1, pressure  how hard the held strings are bowed
 *   CC64           sustain
 *   CC74           TONE, darker / brighter;  CC91 more / less reverb
 *   CC120, CC123   everything let go
 *   other CCs      the important settings (ui_midi_cc; see docs/MIDI.md)
 *   clock          the tempo follows it (fitted over the last four beats of
 *                  ticks), its phase held on the audio clock; START,
 *                  CONTINUE, STOP and song position start, stop and place
 *                  the drone (not with START set to IGNORE)
 * Out, to every port: clock (24 a quarter note), START and STOP, while the
 * drone plays and no clock is coming in. The clock goes out as the sound is
 * heard (the audio clock).
 *
 * Which way the clock goes is the MIX page's MIDI setting (GP_MIDI: AUTO as
 * above, SEND always, FOLLOW only, OFF neither). Bricks also follow one
 * another over USB through a hub (link.h).
 * Settings, in launch.sh: JD_MIDI=0 turns it off, JD_MIDI_NOTES=CHANNELS,
 * JD_MIDI_CLOCK=0 never sends clock, JD_MIDI_FOLLOW=0 never follows it. */
#pragma once
#include <stdint.h>

void midi_start(int core);   /* the thread (pinned to core, -1: anywhere) */
void midi_stop(void);
void midi_set_latency(float seconds); /* from the engine's position to the sound heard */
/* messages for the screen ("MIDI: <device>"), main thread */
int midi_take_notice(char *buf, int n);
int midi_following(void);    /* an outside clock leads */
int midi_ports(void);        /* the MIDI ports open, links included */
int midi_links(void);        /* Bricks following this one over the USB clock link (link.h) */
/* a message for the screen, any thread (fmt with one %s) */
void midi_notice(const char *fmt, const char *arg);
/* the next CC that isn't one of the fixed ones above (for ui_midi_cc), main
 * thread; 0 when there's none */
int midi_take_cc(int *ch, int *cc, int *v);

/* the parser and its actions without the thread or devices, for the test
 * harness: bytes in, as if from a port, at a time in microseconds */
void midi_feed(const uint8_t *b, int n, uint64_t now_us);
void midi_reset(void);
/* the leading Brick's position for the ones following over the link: its
 * bytes (F0 7D 'T' 'L' ... F7, see midi.c), and when they are due (every 20 ms
 * while the clock goes out; 0 when not) */
int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing);
int midi_link_step(uint64_t now_us, uint8_t *out, int max);
int midi_voice_of(int ch, int note); /* the voice a held key plays, or -1 */
/* the clock out, stepped by hand: returns the bytes it would send now */
int midi_clock_step(uint64_t now_us, uint8_t *out, int max);
