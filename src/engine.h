/* Jaffedrone audio engine: five Karplus-Strong strings kept ringing by a
 * noise bow and plucked by a Euclidean rhythm each, tuned to a scale (equal,
 * just, raga, gamelan), coupled through a shared bridge (sympathetic
 * resonance) into a resonant body, with two beat-synced LFOs, a tempo-synced
 * delay, a reverb, and Wi-Fi tempo sync compatible with HI5. Chord changes
 * follow a progression that fits the scale. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define JD_VERSION "0.13"

#define NS 5
#define NSP 28 /* params per string */
#define NGP 32 /* global params */
#define NPROG 16 /* chords in the progression memory */
#define NPRESET 10
#define NLFO 2
#define NLP 4  /* params per LFO */
#define NFX 18 /* effect params */
#define NSYMP 12 /* most sympathetic strings */

/* SP_NOTE is a scale degree above the root (in EQUAL, a semitone). Each
 * string has two Euclidean rings sharing one step length (SP_RATE): the
 * pluck ring plucks on its pulses, the bow ring swells the bow on its pulses
 * (off: the bow plays continuously). Ring fields: on, steps, pulses, rotate. */
enum { SP_NOTE, SP_FINE, SP_LEVEL, SP_BOW, SP_DECAY, SP_BRIGHT, SP_FOLD, SP_STIFF, SP_POS, SP_BUZZ,
       SP_PON, SP_PSTEPS, SP_PPULSES, SP_PROT, SP_BON, SP_BSTEPS, SP_BPULSES, SP_BROT, SP_RATE, SP_PAN,
       SP_PCHANCE, SP_BCHANCE, SP_HARM, SP_GATE, SP_VOICE, SP_RELEASE, SP_LATK, SP_LISTEN };
/* SP_PCHANCE / SP_BCHANCE: the chance each pulse of the ring plays.
 * SP_HARM: a light touch at a node (OFF, or the 2nd .. 5th harmonic).
 * SP_GATE: how far the string's sound follows its own bow ring.
 * SP_VOICE: what the voice is: a string (0) or a wind voice (1 + its
 * exciter), so strings and winds can play side by side.
 * SP_RELEASE: in LATCH, how long the string takes to fade when let go.
 * SP_LATK: in LATCH, how long a bowed (or blown) string takes to swell in. */
enum { RING_PLUCK, RING_BOW, NRING };
#define RING_P(ring, field) (SP_PON + (ring) * 4 + (field)) /* field 0 on, 1 steps, 2 pulses, 3 rotate */
#define NSOUND SP_PON /* sound params come first */
/* MOTION: small steps .. bold leaps; HOME: pull back to the tonic; COLOR:
 * sevenths and suspensions; EVOLVE: chance a chord change also nudges a
 * string's rhythm */
enum { GP_MODE, GP_SCALE, GP_ROOT, GP_CHANGES, GP_BPM, GP_DRIFT, GP_GLIDE, GP_CUTOFF, GP_MASTER, GP_SYNC,
       GP_MOTION, GP_HOME, GP_COLOR, GP_EVOLVE, GP_CHANCE, GP_RELATE, GP_ENGINE, GP_EXCITE, GP_MATERIAL,
       GP_SUB, GP_SUBDIV, GP_SUBRES, GP_ATTACK, GP_RELEASE, GP_KEYLOCK, GP_MODALITY, GP_PROG, GP_PLAYMODE,
       GP_OUTPUT, GP_CLKSIDE, GP_PPQN, GP_PULSE };
/* GP_ATTACK / GP_RELEASE: how the drone fades in and out with START.
 * GP_KEYLOCK: for playing with others: the root, scale and mode stay put and
 * chords are tuned exactly. GP_MODALITY: the mode chords are drawn from in
 * EQUAL (AUTO is Dorian). GP_PROG: FREE (the algorithm), RECORD (each chord
 * is kept, up to NPROG) or PLAY (the kept chords, in turn, round and round).
 * GP_OUTPUT: STEREO, or MONO + CLOCK: the sound in mono on one side and
 * clock pulses on the other (GP_CLKSIDE: 0 = clock on the right), GP_PPQN
 * pulses per quarter note (an index into 1, 2, 4, 8, 16, 24, 32), GP_PULSE
 * the pulse width in ms. In MONO + CLOCK every pan and stereo spread plays
 * from the centre (the settings are kept). */
enum { OUT_STEREO, OUT_MONO_CLOCK };
#define NPPQN 7
enum { MODAL_AUTO, MODAL_IONIAN, MODAL_DORIAN, MODAL_PHRYGIAN, MODAL_LYDIAN, MODAL_MIXOLYDIAN, MODAL_AEOLIAN,
       MODAL_LOCRIAN, NMODAL };
enum { PROG_FREE, PROG_RECORD, PROG_PLAY, NPROGMODE };
/* GP_PLAYMODE: RINGS (the Euclidean rings play the strings) or LATCH (the
 * rings rest; each string sounds when plucked or bowed by hand and drones
 * until it is played again, then fades over its SP_RELEASE) */
enum { PLAY_RINGS, PLAY_LATCH, NPLAYMODE };
enum { LATCH_OFF, LATCH_PLUCK, LATCH_BOW, LATCH_HAMMER }; /* HAMMER: started by a tap */
/* GP_SUB*: every voice can sound undertones: its pitch divided (an octave,
 * a twelfth or two octaves below, or both octave and twelfth), as a
 * resonance that rings (SUB RES) under the voice. */
enum { SUB_OCT, SUB_TWELFTH, SUB_TWO_OCT, SUB_OCT_TWELFTH, NSUBDIV };
/* A wind voice (SP_VOICE > 0) is a bore (a waveguide) driven by its exciter
 * and made of GP_MATERIAL (the BORE); its BOW, DECAY, STIFF, POS, BUZZ and
 * HARM slots become BREATH, AIR, EMBOUCH, VIBRATO, GROWL and REGISTER, the
 * pluck ring tongues and the bow ring breathes. GP_ENGINE and GP_EXCITE
 * are no longer used; they are kept so older songs load. */
enum { ENG_STRINGS, ENG_WIND, NENGINE };
enum { EX_LIP, EX_REED, EX_DOUBLE, EX_FLUTE, EX_FREE, EX_RECORDER, EX_BANSURI, NEXCITE };
/* A struck voice (SP_VOICE > NEXCITE) is a vibrating body of resonant
 * modes, struck by a mallet (a mass on a spring that meets the surface and
 * bounces off) or bowed (stick-slip friction). Its STIFF, POS, BUZZ, DECAY,
 * BRIGHT and HARM become STRETCH, SPOT, SIZZLE, RING, MALLET and SING. */
enum { PC_GENDER, PC_BONANG, PC_GONG, PC_BOWL, PC_HANDPAN, PC_STEELPAN, PC_TONGUE, PC_CYMBAL, PC_DRUM,
       PC_GLASS, PC_CHORD, PC_ALIEN, NPERC };
#define VOICE_PERC(p) (1 + NEXCITE + (p)) /* the SP_VOICE of struck model p */
enum { MAT_WOOD, MAT_METAL, MAT_CERAMIC, MAT_GLASS, NMATERIAL };
/* GP_CHANCE: the chance a scheduled change happens. GP_RELATE: how many
 * notes the next chord must share with the last (0 .. 2). */
enum { LP_RATE, LP_SHAPE, LP_TARGET, LP_DEPTH };
/* FX_S*: the sympathetic strings: how many, how they are tuned, their
 * octave, level and ring time */
/* FX_BIN*: binaural. The strings are shifted down in the left ear and up
 * in the right by half the beat each, so every partial beats between the
 * ears. BEAT in half hertz (0 = off), DEPTH, LOCK (beat locked to tempo). */
enum { FX_DTIME, FX_DFB, FX_DMIX, FX_VSEND, FX_VTIME, FX_VTONE, FX_BINBEAT, FX_BINDEPTH, FX_BINLOCK, FX_SYMP, FX_BODY,
       FX_BTYPE, FX_SCOUNT, FX_STUNE, FX_SOCT, FX_SLEVEL, FX_SRING, FX_AIR };
/* FX_BINLOCK (MODE): FREE, TEMPO (locked to the beat), two journeys, and
 * OMBAK, the beat played in the air for both ears, as Balinese paired
 * instruments beat (pengumbang and pengisep) */
enum { BIN_FREE, BIN_TEMPO, BIN_AT, BIN_TD, BIN_OMBAK, NBINMODE };
enum { STUNE_SCALE, STUNE_CHORD, STUNE_CHORD9, STUNE_DRONE, STUNE_FIFTHS, NSTUNE };
/* body TYPE: METAL, CERAMIC and GLASS are the bore materials as a body */
enum { BODY_WOOD, BODY_GOURD, BODY_BRONZE, BODY_TUNED, BODY_METAL, BODY_CERAMIC, BODY_GLASS, NBODY };
enum { MODE_MONO, MODE_CHORD };
enum { SYNC_OFF, SYNC_AUTO, SYNC_LEAD, SYNC_FOLLOW, NSYNCMODE };
enum { LT_CUTOFF, LT_LEVEL, LT_PITCH, LT_BOW, LT_DELAY, LT_VERB, NLT };
enum { SH_SINE, SH_TRI, SH_SAW, SH_SQUARE, SH_RANDOM, NSH };

typedef struct {
    const char *name;
    int min, max;
} ParamDef;

typedef struct {
    uint32_t magic;
    int on[NS];
    int sp[NS][NSP];
    int gp[NGP];
    int lfo[NLFO][NLP];
    int fx[NFX];
    /* the progression memory: each chord's notes, fine tuning and name */
    int prog_n, prog_pos;
    int prog_note[NPROG][NS], prog_fine[NPROG][NS];
    char prog_name[NPROG][24];
} Song;

/* shared with the UI; the audio thread publishes meters and the beat */
typedef struct {
    int playing;
    volatile float meter[NS];
    volatile float master_meter;
    volatile uint32_t beat;
    volatile float phase[NS];  /* string animation phase */
    volatile float lfo_val[NLFO]; /* 0..1 for the display */
    volatile float bin_hz;        /* the binaural beat now, in Hz */
    volatile int step[NS][NRING];   /* current step of each ring */
    volatile float flash[NS][NRING]; /* 1 on a hit, fading (display) */
    volatile float bow_env[NS];     /* how open each bow is now */
    volatile float hand_env[NS];    /* the hand bow alone */
    volatile float symp_meter;      /* sympathetic strings, all together */
    volatile uint32_t chords;     /* counts chord changes (manual and CHANGES) */
    volatile int latch[NS];        /* LATCH: how each string was started (LATCH_OFF when let go) */
    volatile int rate;
} Live;

/* counters for the log, filled by the audio thread and read (and reset)
 * by the main thread; a torn read only blurs one status line */
typedef struct {
    volatile uint32_t frames, clips;
    volatile float peak, rms_acc, lev_min, lev_acc;
    volatile float f_target[NS], f_meas[NS], meter[NS], breath[NS], corr[NS];
    volatile int detail; /* measure each voice's pitch */
} EngineStats;

extern Song song;
extern Live live;
extern EngineStats stats;
void engine_stats_reset(void);
float engine_voice_pitch(int v); /* from its recent sound (log DETAIL only) */

const ParamDef *sparam_def(int p);
const char *sparam_name(int s, int p); /* the name for voice s's engine */
int engine_voice_wind(int s);          /* 1 if voice s is a wind voice */
int engine_voice_perc(int s);          /* 1 if voice s is a struck voice */
extern int engine_mallet_lands; /* the dropped mallets' landings after the strike (tests) */
int engine_perc_count(const Song *g);  /* how many voices are struck */
extern unsigned engine_perc_clamps; /* samples a struck voice's limiter engaged (diagnostics) */
extern int engine_perc_skip;  /* skip struck modes that have died away (on; off only in tests) */
extern float engine_air, engine_room; /* AIR: how strongly listeners hear (0 .. 1, from FX_AIR); room unused */
extern int engine_listen;           /* AIR: a bit for each listening voice (from SP_LISTEN) */
enum { AIR_LISTEN, AIR_FLOW };
enum { AIR_SENDS, AIR_LISTENS, AIR_OUT }; /* SP_LISTEN: a voice plays into the air, only listens, or stays out */
extern int engine_air_mode;         /* AIR: AIR_LISTEN (used) or AIR_FLOW (kept for later, not exposed) */
extern int engine_perc_cache; /* keep struck modes between sub-blocks (on; off only in tests) */
int engine_wind_count(const Song *g);  /* how many voices are winds */
const char *engine_voice_name(int v);  /* an SP_VOICE value: STRING, LIP .. */
const ParamDef *gparam_def(int p);
const ParamDef *lparam_def(int p);
const ParamDef *fparam_def(int p);
void sparam_format(int s, int p, char *b, int n);
void gparam_format(int p, char *b, int n);
int clk_ppqn_value(int i); /* GP_PPQN index -> pulses per quarter note */
void lparam_format(int l, int p, char *b, int n);
void fparam_format(int p, char *b, int n);
void note_name(int midi, char *b, int n);
const char *sync_mode_name(int m);

void engine_init(int sample_rate);
void engine_render(int16_t *out, int frames);
void engine_set_playing(int on);
void engine_pluck(int s);
/* a tap: a string is struck like a hammered dulcimer (a bright hammer hit
 * with a light bounce), a wind voice gets a hard overblown attack */
void engine_hammer(int s);
void engine_bow(int s, int on); /* bow by hand while held, over the rings */
void engine_overblow(int s, int on); /* while bowing: overblow (wind) or touch a harmonic (strings) */
/* LATCH: a pluck or bow (LATCH_PLUCK, LATCH_BOW) starts string s droning;
 * played again, it fades over its RELEASE. Returns the new state. */
int engine_latch(int s, int how);
void engine_unlatch_all(void); /* every string lets go */
void engine_next_chord(void);  /* the next chord of the progression */
void engine_set_scale(int sc); /* retune: each string moves to the nearest degree */
void engine_euclid_random(int s, int ring); /* a new simple rhythm for one ring */
int euclid_hit(int steps, int pulses, int rot, int step);
int engine_nscales(void);
const char *engine_scale_name(int sc);
const char *engine_chord_name(void); /* the current chord, in the scale's own names */
void engine_degree_name(int degree, char *b, int n); /* e.g. "Pa'" or "E3-14" */
const char *engine_chord_history(int i);  /* i chords ago (0 = now), "" if none */
int engine_scale_n(void);                 /* degrees per octave */
float engine_scale_cents(int i);          /* degree i (0..n-1) in cents */
void engine_pc_name(int pc, char *b, int n); /* degree name without octave */
int engine_symp_degree(int k);            /* where sympathetic string k is tuned */
float engine_symp_meter(int k);

/* transport for wifi sync: position in 16th steps with fraction, a hard
 * jump, a playback speed trim (1.0 = none) and a float tempo */
double engine_position(void);
void engine_set_position(double steps);
void engine_set_rate(double r);
float engine_bpm(void);
void engine_set_bpm(float bpm);

void song_default(void);
int song_peek(const char *path, Song *out); /* read a song without playing it */
void engine_prog_mode(int mode);           /* RECORD starts afresh from the chord now */
int song_save(const char *path);
int song_load(const char *path);
