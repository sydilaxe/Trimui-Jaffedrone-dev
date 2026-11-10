/* MIDI over USB: class-compliant keyboards, controllers, sequencers and
 * drum machines on the TrimUI's USB-C port (an OTG adapter or hub), read and
 * written through ALSA's raw MIDI ports (/dev/snd/midiC*D*), so nothing more
 * is needed on the device. Ports are found as they are plugged in.
 *
 * In (any channel):
 *   notes          a key plays the voice its channel plays (the MIDI
 *                  page: each voice may have a channel of its own; the keys
 *                  on the other channels, or on KEYS's, play the voice
 *                  selected on PLAY) at the key's own pitch, on a key voice
 *                  of its own (engine_key_voice), beside the strings, so the
 *                  drone and the song are untouched: NKEY at once, at most
 *                  POLY a voice. PLAY: it plucks as hard as the key is
 *                  struck (VEL: how much that matters) and bows while held
 *                  (a wind: tongues and blows), or only plucks, bows or
 *                  strikes. HOLD: while held, or as a latch (SONG: as PLAY
 *                  MODE). SHIFT moves every key, SNAP onto the song's
 *                  scale. A key never starts the drone: it sounds with it
 *                  stopped.
 *   pitch bend     BEND semitones (the MIDI page)
 *   aftertouch     key or channel: what AFTERTOUCH moves (bow, tone, swell,
 *                  vibrato, pitch, harmonic, fold, buzz)
 *   CC1            what MOD WHEEL moves (the same choice)
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
 * Settings, in launch.sh: JD_MIDI=0 turns it off, JD_MIDI_NOTES=CHANNELS
 * gives voices 1-5 channels 1-5 (unless the MIDI page has set channels),
 * JD_MIDI_CLOCK=0 never sends clock, JD_MIDI_FOLLOW=0 never follows it. */
#pragma once
#include <stdint.h>

#include "engine.h"

/* the MIDI page's settings (ui.c shows, edits and keeps them; they belong to
 * the Brick, not the song) */
enum { MT_OFF, MT_BOW, MT_TONE, MT_SWELL, MT_VIBRATO, MT_PITCH, MT_HARM, MT_FOLD, MT_BUZZ, NMT };
/* how a key plays: PLAY, what it does to the voice (both pluck and bow, as
 * the voice's hand playing does; a pluck that rings on; a bow only; the
 * mallet), HOLD, how long it sounds (as PLAY MODE does for the hands:
 * RINGS held, LATCH till let go; GATE always while held; LATCH a press
 * starts it droning and the key again lets it go) */
enum { KP_BOTH, KP_PLUCK, KP_BOW, KP_STRIKE, NKP };
enum { KH_SONG, KH_GATE, KH_LATCH, NKH };
typedef struct {
    int vch[NS];       /* each voice's own channel: 0 none, 1-16 */
    int keys_ch;       /* the selected voice's channel: 0 any not a voice's, 1-16 */
    int vel;           /* VELOCITY: 0 every key alike .. 100 as struck */
    int at_to, mod_to; /* what aftertouch and the mod wheel move (MT_) */
    int bend;          /* pitch bend range, semitones */
    int play, hold;    /* KP_, KH_ */
    int shift;         /* semitones added to every key */
    int snap;          /* 1: keys move to the nearest note of the song's scale */
    int poly;          /* the most keys a voice plays at once (1: one, the last) */
} MidiCfg;
extern MidiCfg midi_cfg;
const char *midi_target_name(int t);
int midi_channel_voice(int ch); /* the voice channel ch (0-15) plays: its own, the selected one, or -1 */

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
