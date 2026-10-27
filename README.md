# Jaffedrone

A five-string drone instrument for the Miyoo Mini Plus running OnionOS. It joins an organic string drone
to an algorithmic progression. You can play it as an instrument, or let it run as a slowly changing drone
for meditation.

**Manual:** [docs/Jaffedrone-Manual.pdf](docs/Jaffedrone-Manual.pdf) shows each command on a drawing of the Miyoo, with the app's screens (`make manual` rebuilds it).

## The strings

Each string is a Karplus-Strong loop with the Jaffe-Smith extensions:

- an allpass for fine tuning;
- a damping filter for brightness;
- a loop gain for decay.

A noise "bow" keeps the string ringing, held in check by the loop's own level. Per string:

- **FOLD:** a wavefolder on the string's output.
- **STIFF:** dispersion, for an inharmonic, bell-like tone.
- **POS:** the pick position.
- **BUZZ:** a jawari-like bridge. A strong note buzzes, and the buzz blooms and fades as the note decays, as
  on a tanpura or sitar.
- **HARM:** a light touch at a node, like a harmonic on a guitar or a flageolet on a violin. Touching at a half,
  a third, a quarter or a fifth of the string brings out the 2nd, 3rd, 4th or 5th harmonic, which blooms out
  of the note with a glassy tone.

Shared by all five strings (SOUND page, bottom rows):

- **COUPLE:** sympathetic resonance. The strings share a bridge, so each one hears the others, and an
  untouched string tuned to a harmonic starts to ring.
- **BODY:** five resonant modes under the strings. Choose a body type:
  - **WOOD:** a wooden box.
  - **GOURD:** a gourd, like a tanpura's.
  - **BRONZE:** rings inharmonically, like a gong.
  - **TUNED:** resonates on the overtones.
  - **METAL, CERAMIC, GLASS:** the wind materials as a body (see BORE below).

  BRONZE, TUNED and METAL follow the root note.
- **BORE:** what the wind voices are made of (see Wind).
- **TONE:** the master lowpass.

## Wind

Each voice can be a string or a wind. The **VOICE** row at the top of the SOUND page sets it per column:
STRING, or one of the seven wind voices below. So voice 1 can be a bowed string and voices 2 and 3 a reed and
a bansuri. The rows under each column follow its voice: a wind column shows BREATH, AIR, EMBOUCH and so on,
and its rings show TONGUE and BREATH on the PLAY page. Everything else is shared: the progressions, the
sympathetic strings, the mix and the effects. Changing a voice while it sounds starts it afresh with a short
fade in.

A wind voice is a bore: a waveguide closed by a loss filter, driven at the mouthpiece by an exciter:

| Exciter | Like | How it sounds |
|---|---|---|
| LIP | brass, conch, didgeridoo | the lips buzz near the note and let breath through as they open |
| REED | clarinet | a single vibrating reed on a cylindrical bore: hollow, odd harmonics |
| DOUBLE | oboe, shehnai, duduk | a stiffer double reed on a full bore: reedy, nasal, all harmonics |
| FLUTE | concert flute | an air jet blown across an edge |
| FREE | sheng, harmonium | a free reed sounding at its own pitch into a short pipe |
| RECORDER | recorder | a duct-guided jet: steady and sweet, with a chiff on each attack |
| BANSURI | Indian bamboo flute | a longer, breathier transverse jet |

**BORE** (the shared row) sets what every wind voice is made of: the walls' losses and a little wall
dispersion. The body TYPE has the same materials, for a body that matches.

- **WOOD:** warm and soft.
- **METAL:** bright; as a body, it rings at the root.
- **CERAMIC:** an earthen hollow, like an ocarina.
- **GLASS:** pure and sparkling, with high, long resonances.

A wind voice's controls take their wind meanings:

| Strings | Wind | |
|---|---|---|
| BOW | BREATH | steady blowing pressure (0 = only tongued notes and the hand) |
| DECAY | AIR | breath noise |
| STIFF | EMBOUCH | lip tension, reed opening or jet length: the edge of the tone |
| POS | VIBRATO | breath vibrato, a little different on each voice |
| BUZZ | GROWL | a rasp in the breath |
| HARM | REG | the register: 1st to 5th |

The pluck ring becomes **TONGUE**: each pulse is a tongued note. The tongue stops the air for an instant,
then a quick push of breath comes through with a burst of breath noise; lips buzz harder at the start, a
growl blooms, and the flutes chiff. The note holds for most of a step and falls back. The bow ring
becomes **BREATH**: it swells the blowing. Flutes, recorders and bansuris sound an octave higher than the
strings, as flutes do. Each voice is checked by the test harness: every exciter in every material plays in
tune within a few cents over three octaves. The lip voice listens to itself and lips its pitch onto the note,
as a player does.

## Struck voices

The VOICE row also offers twelve **struck** voices: each a physical model of a vibrating body, its resonant
modes struck by a mallet or bowed. **R2 + Y / X** on VOICE jumps between the families (strings, winds,
struck); Y / X alone step one at a time.

- **The mallet** is a mass on a spring that meets the surface where it strikes, pushes while compressed and
  leaves on its own: a hard mallet makes a short, bright contact, a soft one a long, dark push, and it can
  bounce back for a second touch. L2 drops a hard mallet that bounces, each rebound lower and sooner.
- **The bow** is stick-slip friction at a point: the surface catches on the bow and slips, and the modes it
  meets grow until they sing, as a rubbed singing bowl or a bowed cymbal does.
- **The touch** (L1 + R1, or B in LATCH): a finger damps the fundamental; the next mode rings and the bow
  sings it.

| Voice | | Modes, over the fundamental |
|---|---|---|
| GENDER | gamelan metallophone bars | rectangular-bar modes 1, 2.76, 5.40, 8.93 and the harmonic partials makers shape in (Sethares); paired, beating (ombak) |
| BONANG | gamelan kettle gongs | 1, 1.52, 3.46, 3.92 (as commonly cited); paired |
| GONG | a large gong | low partials, pairs beating near 3 Hz as measured on a Balinese gong ageng |
| BOWL | a singing bowl | 1, 2.9, 5.7, 9.1 (Inácio, Henrique and Antunes), split pairs beating slowly |
| HANDPAN | a handpan note | 1 : 2 : 3, fundamental, octave and compound fifth, as tuned |
| STEELPAN | a steel pan note | 1, 2, 3, 4, 5, brighter |
| TONGUE | a steel tongue drum | a bar clamped at one end: 1, 6.27, 17.5, so nearly pure |
| CYMBAL | a cymbal | a thin plate's dense series, about n^1.5, as measured |
| DRUM | a drum | an ideal membrane: 1, 1.59, 2.14, 2.30, 2.65, 2.92, 3.16, 3.50 |
| GLASS | a glass harmonica, a crystal glass | a thin pure shell, long and made to be rubbed |
| CHORD | no instrument: a metal body tuned to the scale | its modes are the current scale's notes stacked in thirds, so it rings as a chord in tune with the drone |
| ALIEN | no instrument: an impossible metal | modes a golden ratio apart, evenly spaced in pitch yet never harmonic, shimmering slowly |

They are physical models, but not limited to the instruments they start from:

- **RING** runs from a short thud to ten minutes. Above 95 a struck voice becomes a **metal drone** that
  hardly fades (the display reads DRONE at the top), and at long rings the upper modes keep their shimmer
  almost as long as the fundamental.
- **STRETCH** pulls the partials apart, gently at first and far at the top, into clangor no body has.
- FOLD, SPOT, SING and the bow push further still: a bowed ALIEN, a stretched CHORD struck hard, a glass
  bowed on its third mode.
- Each struck body has its **own limiter**: strike after strike on a long ring cannot pile up past a ceiling;
  its peaks are held and its modes ease down together, so the tone stays.

A struck voice's controls: **RING** (how long it rings, in seconds; DRONE at the top), **MALLET** (soft felt to
hard), **STRETCH** (pulls the modes apart: more clangor), **SPOT** (where it is struck and bowed: which modes speak), **SIZZLE** (a rattle
that follows the ring), **SING** (the mode the bow favours) and **BOW** (bow pressure). The pluck ring
strikes and the bow ring bows; LATCH, RELEASE and ATTACK work as for strings.

## AURA: voices that listen

AURA lets some voices sound only from the others, as if they stood side by side in one room: a gong humming
along with a flute, bowls singing when a reed plays, a string ringing in sympathy. On the MIX page each
voice has an **AURA** setting:

- **SEND**: it plays, and its sound goes into the air (the default).
- **LISTEN**: it is never bowed, blown, struck or plucked (its rhythms, the shoulders and LATCH leave it
  alone); it sounds only from what reaches it.
- **OFF**: it plays, out of the air.

**AMOUNT**, under AURA at the left of the effects, sets how strongly the listeners respond (OFF to 100).
Each voice stands where its PAN puts it, across a stage of about 3 m: a player's sound reaches a listener
later (343 m/s) and fainter the further apart they are. A listener rings where its own modes meet the
players' notes, so struck bodies with a long RING (bowls, gongs, glass) sing most; a wind makes a faint
listener. The sound goes one way only: listeners hear the senders and the listeners before them (by voice
number), never the other way, and a listener passes nothing onto the bridge, so nothing can feed back.
With every voice on SEND, AURA does nothing. Delay, reverb and binaural come after, on everything.
`tools/proto/air_sweep.c` checks hundreds of random combinations for runaway; `air_requested.c` renders
example ensembles.

## Undertones

SUB (MIX page) gives every voice, string or wind, undertones that ring beneath it. Every n-th cycle of the
voice strikes a resonance n times lower, so the undertone follows the voice's pitch and dynamics.

- **SUB:** how loud. At high settings the undertone is lightly saturated, adding overtones so it is heard
  even on the Miyoo's small speaker, which plays little below about 200 Hz. Headphones bring out the full
  depth.
- **SUB DIV:** an octave below (OCT), a twelfth below (12TH), two octaves below (2 OCT), or the octave and the
  twelfth together (OCT+12).
- **SUB RES:** how long the undertones ring, from a short thump to a long resonant hum.

## Sympathetic strings

Up to 12 extra strings that are never played. Like a sitar's taraf, they ring only when the main strings
sound their pitches through the bridge, adding a halo around the drone. On the SOUND page:

- **SYMP:** how many (0 turns them off).
- **S TUNE:** how they are tuned. In the two chord modes they retune by themselves at every chord change.
  - **SCALE:** the degrees of the scale, rising.
  - **CHORD:** the notes of the current chord.
  - **CHORD+:** the chord plus the scale tone above each chord note (9ths, 11ths, 13ths).
  - **DRONE:** root and fifth, like a tanpura.
  - **FIFTHS:** a stack of fifths from the root.
- **S OCT:** their octave, from one below the root to two above.
- **S LEVEL** and **S RING:** how loud they are, and how long they ring.

Each tuning repeats within two octaves. A second pass over the same pitches is detuned a few cents, so doubled
strings shimmer against each other.

## Latch: play each string by hand

PLAY MODE (START + A, or on the SONG page) switches between **RINGS**, where the Euclidean rings play the
strings, and **LATCH**, where the rings rest and you play:

- Every string starts silent. On PLAY, pick a string with left and right, then **L1** plucks it or **R1**
  bows it (a wind voice: tongue or blow). Either way it keeps droning, sustained by the bow or breath, with
  no need to hold the button.
- Press **L1 or R1 again** on that string and it lets go, fading over its own **RELEASE**: 0.05 s for a
  clipped stop up to 20 s for a long dying ring. Up and down set the selected string's RELEASE (it is also
  on the MIX page, in place of GATE).
- A pluck starts at once; a bow swells in over the string's **ATTACK** (0.05 to 10 s, 1.8 s at first): **Y /
  X** make the selected string's ATTACK shorter / longer. **A** lets every string go.
- Hold **B**, or **L1 and R1** together (either first), and the selected droning string sounds its next
  harmonic up (a wind voice overblows); let go and it comes back. **L2** drops the mallet on it instead
  of plucking.
- Each column shows a board with a shed-roof top that fills while its string drones and drains as it fades,
  with the word PLUCKED, BOWED or FADING.

LATCH and the RELEASE times are saved with songs and presets.

## Rhythm: two rings per string

Each string has two Euclidean rings (steps, pulses, rotation) that share one step length, from 1/16 to two bars:

- **The pluck ring** plucks the string on its pulses. The first pulse of each cycle is a little stronger.
- **The bow ring** swells the bow in and out on its pulses, so the string breathes. With the bow ring off, the
  bow plays continuously.

Each ring can be switched on and off and has a **CHANCE**: how often each pulse actually plays. Below 100%
the patterns breathe instead of looping exactly. The rings are meant to be simple and slow, and each string
keeps its own.

Each string also has a **GATE** (MIX page): how far its sound follows its own bow ring. At 0 the bow ring
only shapes the bowing; at 100 it gates the string's level as well. This replaces the old master gate.
You can always pluck or bow a string by hand on top of the rings, and mute any string from the PLAY page.

## Tunings and progressions

`SCALE` (DRONE page, or R2 + left/right on PLAY) sets the tuning. Each string's `NOTE` counts degrees of the
scale above the root.

| Scale | Tuning | What SELECT and CHANGES do |
|---|---|---|
| EQUAL | 12-tone equal temperament | chords from the Dorian mode |
| JUST MAJOR, JUST MINOR | 5-limit just intonation | chords stacked from the scale |
| HARMONICS | the 8th to 15th overtones | the same; sevenths are pure 4:5:6:7 |
| PENTA JUST, SEPTIMAL | just pentatonics (5-limit, and 7-limit with 7/6 and 7/4) | the same |
| YAMAN, BHAIRAV, KAFI, BHUPALI, MALKAUNS | ragas in just (shruti) ratios | a tanpura below (Sa and Pa, or Ma for Malkauns); the upper strings wander a step or two at a time |
| SLENDRO, PELOG, SELISIR | Javanese and Balinese gamelan tunings | a gong tone, a kempyung pair and a beating unison pair (ombak) move in parallel |
| BEGBEG, SEDANG, TIRUS | Balinese gong kebyar (see below) | the same as the gamelan tunings |

**Balinese tunings.** No two Balinese gamelan are tuned alike, but gong kebyar tunings fall along a range
that Andrew Toth described from measuring every key of 49 gamelan across Bali. Three profiles mark it, in cents
above ding (as reproduced by Vitale and Sethares,
[Balinese Gamelan Tuning: The Toth Archives](https://iftawm.org/journal/oldsite/articles/2021b/Vitale_Sethares_AAWM_Vol_9_2.pdf)):

| | ding | dong | deng | dung | dang |
|---|---|---|---|---|---|
| BEGBEG | 0 | 120 | 234 | 666 | 747 |
| SEDANG | 0 | 136 | 291 | 670 | 804 |
| TIRUS | 0 | 197 | 377 | 724 | 828 |

BEGBEG has the closest small steps and the widest gaps; over the 20th century many gamelan were retuned
toward it. TIRUS is the older, more even shape, and SEDANG lies between. Balinese instruments are also
played in pairs tuned a few hertz apart (pengumbang and pengisep), whose beating, *ombak*, gives the
shimmer: Toth's gamelan beat at about 6 to 10 Hz. Binaural MODE OMBAK plays that beat for both ears.

How the chord progressions move, and the COMPOSE controls that shape them:

- **Root motion** is mostly by fourths and fifths. **MOTION** sets the rest: low favours thirds, which keep two
  notes in common, so changes are calm; high favours steps and distant moves. For ragas it sets how far a
  voice may wander; for gamelan, how far the block moves.
- **Chord types:** triads, sevenths and suspensions. **COLOR** sets how often sevenths and suspensions
  appear; for gamelan, how widely the beating pair is tuned apart.
- **Going home:** the further the progression wanders, the likelier it returns to the tonic. **HOME** sets
  how strong that pull is; for ragas it draws voices back to Sa and Pa.
- **Voice leading:** each string moves to the nearest chord tone in its own register. Common tones stay, and
  the bass sometimes holds as a pedal.
- **EVOLVE:** the chance that a chord change also nudges one string's rhythm: a ring rotates, gains or loses
  a pulse or a step, or a bow ring starts or stops breathing.
- **CHANCE:** the chance that a scheduled change (CHANGES) actually happens, so the harmony sometimes stays
  longer.
- **RELATE:** how many of the notes now sounding the next chord must keep: FREE, 1 TONE or 2 TONES. With 1
  or 2, every change grows out of the one before. For ragas, RELATE holds that many upper voices still; for
  gamelan, the block moves by kempyung so one tone stays.

Note names are shown in each scale's own language:

- Western scales: note names with the cents offset from equal temperament, for example `C#3-14`.
- Ragas: sargam (`Sa Re Ga MA Pa`). Lowercase is komal, `MA` is tivra, `'` is the octave above and `,` the
  octave below.
- Gamelan: `ji ro lu`… for Javanese and `ding dong deng dung dang` for Balinese.

Changing the scale moves each string to its nearest degree in the new tuning.

- **SELECT:** the next chord.
- **CHANGES:** chords move on their own every 1 to 16 bars (8 by default).

The strings are not re-plucked on a change. They keep ringing and slide to the new notes over the GLIDE time.

## Pages

- **PLAY:** the five strings, each with its pluck ring and bow ring drawn as polygons. The steps are the
  corners, and the pulses are joined into an inner shape that lights on each pluck and fills as the bow swells.
  The band at the top shows the scale, the current chord and the bars until the next change.
- **The quiet view** (START + L2 on PLAY): only the five strings and their rings, for meditation.
- **SOUND:** tone and sound shaping. The engine, exciter and material at the top, then each string's note,
  harmonic touch, bow, decay, brightness, fold,
  stiffness, pick position and buzz, then coupling, body, tone and the sympathetic strings.
- **COMPOSE:** the algorithmic side. On the left, the scale is drawn as a polygon, each degree at its true
  angle in the octave. The current chord is a shape inside it, the sympathetic strings are dots outside, and
  below are the chord, the chords before it and the bars until the next change. On the right: scale, root,
  voicing, changes, CHANCE, glide, tempo, MOTION, HOME, COLOR, EVOLVE, RELATE, drift, Wi-Fi sync and the
  two LFOs.
- **SONG:** ten presets, the chord memory, key lock and mode, and the drone's attack and release.
- **MIX:** each string's level, pan and gate with a meter, master, the sympathetic strings' level, the
  undertones (SUB, SUB DIV, SUB RES), then delay,
  reverb and binaural.

## Song: presets, chord memory, key

The SONG page holds:

- **Ten presets.** Each keeps everything: the sound, the rings, the tuning and the progression memory. Choose a
  tile: **A** loads it, **X** saves the current song to it, and **R2 + Y** clears it. Each tile shows its
  scale, engine and tempo, and a mark if it holds a progression. Loading a preset keeps the Wi-Fi sync and
  output settings.
- **Chord memory.** Set **PROG** to RECORD and each chord change is kept, starting from the chord sounding now,
  up to 16. Set it to PLAY and SELECT or CHANGES steps through the kept chords in order, round and round.
  FREE goes back to the algorithm. R2 + Y on PROG clears the memory. The kept chords are shown as tiles, with
  the one playing highlighted.
- **KEY LOCK and MODE,** for playing with others in a fixed key. MODE chooses the mode the chords are drawn
  from in EQUAL: Ionian, Dorian, Phrygian, Lydian, Mixolydian, Aeolian or Locrian. AUTO is Dorian, as before.
  KEY LOCK keeps the root, scale and mode from changing, even from shortcuts, and tunes every chord exactly
  (no random fine detune, no gamelan beating).
- **ATTACK and RELEASE:** how the drone fades in and out when START starts or stops it, from 0.02 to 10 seconds.
- **OUTPUT: stereo, or mono + clock** for syncing other gear, as on HI5. **MONO+CLK** puts the sound in mono
  on one side of the headphone jack and clock pulses on the other; split it with a stereo-to-two-mono cable.
  **CLOCK ON** picks the side (RIGHT by default), **PPQN** the pulses per quarter note (1, 2, 4, 8, 16, 24 or
  32: 2 for Pocket Operators and Volcas, 24 for DIN sync) and **PULSE** the width (1 to 20 ms, 5 by
  default). The pulses run only while the drone plays, fall on the beat (they leave with the sound, after the
  leveller's few milliseconds of look-ahead) and follow tap tempo and Wi-Fi sync. In mono + clock every
  string's PAN, the sympathetic strings' spread and AURA's seating play from the centre; the settings are kept
  and come back with STEREO. Binaural summed to mono is OMBAK. Presets keep the output settings you have.

## Binaural

Binaural is built into the voices, not added as a separate tone. Their sound below about 1 kHz is shifted
down in the left ear and up in the right by half the beat each, so every low partial of every voice, and
the sympathetic strings, beats between the ears at the same rate. Beats are heard best on carriers of a few
hundred hertz and not at all above about 1 kHz, so the highs are left as they are. Listen on headphones.

- **BEAT:** 0.1 to 40 Hz, in fine steps through the slow bands. The display names the band and the common
  uses: DELTA below 4 Hz (about 3 Hz for deep sleep), THETA 4 to 8 Hz (6 Hz is the most used for
  meditation), 7.83 Hz (the Schumann resonance), ALPHA 8 to 12 Hz (relaxed), BETA and 40 Hz GAMMA. OFF by
  default.
- **DEPTH:** how much of the shifted sound is used.
- **MODE:**
  - **FREE:** the beat as set.
  - **TEMPO:** the beat moves to the nearest power-of-two multiple of the tempo, so it pulses with the
    rhythm.
  - **A>T 10M:** a journey from alpha (10 Hz) down to theta (6 Hz) over ten minutes.
  - **T>D 15M:** from theta (6 Hz) down to delta (2.5 Hz) over fifteen minutes.
  - **OMBAK:** both ears hear the same beat, as a Balinese gamelan's paired instruments beat in the air:
    the low end swells and fades at the BEAT rate, and it works on the speaker without headphones. 6 to
    10 Hz is the gamelan's range.

  Journeys count only while the drone plays.

The research on binaural beats is limited and mixed. These settings are the ones most often used and
studied, not a promise of an effect: 6 Hz theta for meditation (Lavallee et al. 2011), about 3 Hz delta in
sleep (Jirakittayakorn and Wongsawat 2018), and carriers of about 250–400 Hz (see
[a review](https://pmc.ncbi.nlm.nih.gov/articles/PMC6165862/) and
[the sleep study](https://www.frontiersin.org/journals/human-neuroscience/articles/10.3389/fnhum.2018.00387/full)).

## Sound quality

- Each string reads its delay line with four-point Hermite interpolation. Strings keep their brightness on
  every pass instead of being dulled by linear interpolation, and decay evenly across the range.
- The wavefolder is anti-aliased: it averages the fold over each sample from its antiderivative.
- The reverb is an eight-line feedback delay network with damping in every line and slow modulation, for a
  dense tail that doesn't ring. TIME runs from 0.5 to 10 seconds.
- Loudness stays even across settings. A slow leveller keeps the average level steady: it lifts quiet sounds
  (up to 30x) over a few seconds, turns loud ones down faster, and never lifts silence. Then a 6 ms
  look-ahead limiter turns peaks down without thumps. So MASTER is a volume control, not a way to make up
  for a quiet patch. The output is rounded to 16 bits with triangular dither, so quiet tails fade smoothly.
- The reverb and delay hear the sound after TONE, so a dark tone has dark echoes, and four allpasses diffuse
  attacks into a bloom instead of distinct echoes.
- Every delay position is kept within its buffer: a float can't count samples past 2^24 (5.8 minutes) to the
  sample, and that once made the delay click in long sessions.
- Denormal numbers are flushed to zero in the audio thread, so long tails stay cheap on the ARM chip.
- The inner loops avoid library calls: `fmaxf` and `fminf` (calls on the Cortex-A7) are plain compares, the
  wind voices' vibrato, free reed and breath curve use fast polynomial sine and exponential, and the reverb's
  unmodulated lines read at whole samples. About 15% less work than v0.6 on the host.

## Controls

**PLAY**

The rings use the same buttons as HI5's pattern cells and RadioScan's rhythms. The strings are played by hand
on the shoulder buttons.

| Button | Action |
|---|---|
| Left / right | choose a string |
| Up / down | choose the pluck ring / the bow ring |
| A | switch the ring on / off |
| B | fewer pulses, wrapping back to all |
| X | rotate the pattern |
| Y | a new simple pattern |
| L1 | pluck the selected string by hand, the moment it is pressed (WIND: tongue a note) |
| L2 | drop a dulcimer mallet on it: a bright hit, then rebounds, each lower and sooner, until it rests (WIND: a flutter tongue; STRUCK: a hard mallet, bouncing). Held, L2 still shapes the ring |
| R1 (hold) | bow the selected string by hand: the bow is drawn slowly, swelling over about two seconds and brightening as it bites, then sings steadily while R1 is held (WIND: blow) |
| L1 + R1, either first | bow with a harmonic touch: the next harmonic up sings (WIND: overblow, an octave up, a twelfth for REED); bowing with R1, add L1 for overblows as you bow |
| L2 + up / down | more / fewer steps |
| L2 + left / right | step length (right = faster) |
| L2 + X | more pulses |
| L2 + Y | rotate back |
| L2 + A | mute / unmute the string (it fades out and in; a red bar marks it) |
| START + L2 | the quiet view |
| R2 + left / right | previous / next scale |
| R2 + up / down | tone (cutoff) |
| R2 + X / Y | the selected ring's chance more / less (10% steps) |
| R2 + A / B | body more / less |

**Everywhere**

| Button | Action |
|---|---|
| SELECT | next chord |
| START (tap) | start / stop the drone |
| START + L1 / R1 | previous / next page (on SOUND, COMPOSE and MIX, L1 / R1 alone also work) |
| START + A | PLAY MODE: RINGS or LATCH (see Latch) |
| MENU | exit |

**On SOUND, COMPOSE and MIX:** the d-pad moves to the nearest control in that direction, and Y / X lowers or
raises it. On a string's control, A mutes or unmutes that string and B plucks it. Elsewhere, B plucks the
string selected on PLAY. Hand playing only ever plays the selected string; the others keep to their rings.

Songs from earlier versions load as they sounded:

- the scale is set to EQUAL;
- each old PLUCK setting becomes a one-pulse pluck ring with the same timing;
- COUPLE, BODY, CHANGES, EVOLVE, RELATE, binaural and the sympathetic strings are off, and every pulse plays.
- The old master gate is gone, so songs that used it play ungated.

## Audio

The sound is made a buffer ahead by a thread of its own; the audio device only copies it out. The Miyoo's
driver asks for two buffers back to back and then waits, so a sound made inside its request had only half
a buffer's time for each, and past that it crackled for good. Made ahead it has nearly all of it, for one
buffer (21 ms) more delay. The buttons are read every few milliseconds, apart from drawing the screen, so a
press reaches the sound at once. Settings in `launch.sh`, before `./jaffedrone`:

- `JD_BUFFER=512`: smaller buffers, less delay from a press to the sound (1024 by default).
- `JD_AHEAD=0`: make the sound in the driver's request, as before 0.11.3 (to compare).
- `JD_RATE=44100`: a lower sample rate, about 8% less work.

## Log

Jaffedrone keeps a log for tuning the engine, following how it is played, and spotting audio problems:
`jaffedrone_log.txt` in the app folder (it rotates to `jaffedrone_log.txt.1` past 2 MB). **START +
SELECT** cycles it OFF, BASIC and DETAIL; LOG shows in the header while it runs, and the choice is kept in
`jaffedrone.cfg`. It starts at BASIC.

- **Events:** every message shown, parameter changes (repeats while a button is held are merged into one
  line), page changes, chord changes with their scale, and the audio setup at start. DETAIL adds every press
  (with L2/R2 held) and hand playing: plucks, tongued notes, bowing or blowing, with how long.
- **Status:** every 10 s (every second in DETAIL), for example:
  `STAT load avg 4% peak 9% late 0 gap max 22 ms calls 46.9/s of 46.9 | peak 0.71 rms 0.18 clips 0 | leveller avg 3.1 min 1.9 | screen avg 12 ms max 20 ms | audio realtime | cpu 1200 MHz 52 C cores 0-1 | sync lead`

  - **load:** how much of each audio buffer's time rendering took.
  - **late:** audio dropouts: times the sound card ran out of sound. These are crackles.
  - **gap max** and **calls:** the longest wait between two audio callbacks, and how many came each second
    against how many there should be. Fewer calls than wanted, or long gaps at a modest load, mean
    something outside Jaffedrone held the audio up.
  - **peak**, **rms** and **clips:** the output level, and samples near full scale.
  - **leveller:** how much the leveller lifts the sound.
  - **screen:** how long each frame of the screen takes to draw (about 33 ms is one frame at 30 a second).
  - **audio:** `realtime` when the audio thread runs ahead of everything else, as it should on the Miyoo.
  - **cpu:** the processor's clock, its temperature and the cores running. A clock or core count that
    drops mid-session (heat, power saving) explains crackles that the load alone does not.
- **Voices** (DETAIL): each voice's wanted and measured pitch in hertz and cents, its meter, its breath or
  bow, and the lip voice's tuning correction. The pitch is measured from the voice's own sound by the same
  method the test harness uses.
- **Hints:** plain suggestions when something looks wrong: a high load or late callbacks (raise `JD_BUFFER`,
  or use fewer sympathetic strings), clipping (lower MASTER), a leveller lifting at full (raise LEVEL or
  BREATH), or a voice off its note (try another REG, BREATH or EMBOUCH).
- **System report:** the launcher also writes `sysinfo.txt` beside the log: Onion's version and settings,
  the processor, what is running and the audio driver's state at start, then a sample every 10 s while
  Jaffedrone plays (up to an hour).
- **Summary** at exit: presses per button, time on each page, the five most adjusted parameters, and how
  much was played by hand.

The audio thread only fills counters; all writing happens in the main thread.

## Build

- `make` cross-compiles `dist/Jaffedrone/jaffedrone` with zig against Onion's own SDL 1.2.
- The wordmark is drawn from outlines made by `tools/mklogo.py` (`src/logo_data.h`). The start-up screen's
  picture, the Jaffe house's roofline, is made from `art/roofline.jpg` by `tools/mksplash.py`
  (`dist/Jaffedrone/splash.jdi`, a 640x480 picture with 256 colours).
- `make test` renders a WAV and screenshots into `build/test` on the host, and checks:
  - tuning, including just intervals;
  - glide;
  - loading old saves;
  - each tone shaper;
  - coupling and the bodies;
  - the rings;
  - each scale's progression;
  - binaural beats, harmonic touch, chance, RELATE and the string gates;
  - Wi-Fi sync;
  - DSP load, including the worst cases (every effect up) and that resting silent struck modes changes
    nothing that can be heard.
- `make bench` ranks 32 voice mixtures (each voice five times over, and mixtures such as two cymbals, two
  flutes and a free reed) at their worst, heaviest first, relative to the default drone.
- `make install SD=/path/to/card` copies the app into `App/`.
- `make package CROSS=/opt/miyoomini-toolchain/usr/bin/arm-linux-gnueabihf-` builds with the OnionOS gcc
  toolchain instead of zig (in the `aemiii91/miyoomini-toolchain` container) and writes
  `build/Jaffedrone-miyoo.zip`, laid out to unzip at the root of the SD card.

### GitHub builds and releases

Every push runs the **build** workflow:

- It builds the Miyoo package in the OnionOS toolchain container. The zip is kept as the run's
  `Jaffedrone-miyoo` artifact.
- It runs the test harness, checking that there is no clipping, every wind exciter is in tune, the chord
  memory, key lock and presets work, and Wi-Fi sync locks. The output and screenshots are kept as
  `test-output`.

To publish a release with the zip attached, go to **Actions → build → Run workflow** and fill in a tag such as
`v0.11.0`. Pushing a `v*` tag, or creating a release on the website, does the same. The release text comes from
`.github/release-notes.md`.
