# Jaffedrone MIDI implementation chart

Jaffedrone reads and sends MIDI over USB: plug a class-compliant keyboard, controller, sequencer or drum machine
into the TrimUI's USB-C port (through an OTG adapter or hub). Ports are found as they are plugged in. Everything below works
from any page.

## Summary

| Function | Transmitted | Recognized | Remarks |
|---|---|---|---|
| Basic channel | none | 1-16 (any) | Each voice can have a channel of its own (the MIDI page); every other channel (or the KEYS channel, when set) plays the voice selected on PLAY. `JD_MIDI_NOTES=CHANNELS` gives voices 1-5 channels 1-5 until the page sets them. |
| Mode | none | Omni on, poly | |
| Note number | none | 0-127 | The channel's voice at the key's pitch (plus SHIFT, snapped to the song's scale with SNAP), on key voices of their own (eight in all, up to POLY a voice; one more key on a voice takes the place of its key held longest). PLAY: pluck and bow, pluck, bow or strike; HOLD: as PLAY MODE, while held, or latched (the key again lets go). Keys sound with the drone stopped and never start it. |
| Velocity: note on | none | yes | How hard the note is plucked, and the bow's swell; VEL on the MIDI page sets how much it matters. |
| Velocity: note off | none | no | |
| Aftertouch: key | none | yes | Moves what TOUCH picks (below), on that key's note only. |
| Aftertouch: channel | none | yes | Moves what TOUCH picks, on the channel's keys without their own. |
| Pitch bend | none | yes | ±BEND semitones (the MIDI page: 1-24, 2 at first). |
| Control change | none | yes | Fixed: 1, 64, 74, 91, 120, 123 (below). Settings: the CC map (the defaults below, or learned). |
| Program change | none | no | |
| System exclusive | yes | yes | Only the Brick link's position (`F0 7D 'T' 'L' ... F7`, 14 bytes, about 50 a second, to linked Bricks only); others pass by. |
| Song position | none | yes | Places the drone (not with START IN off). |
| Song select | none | no | |
| Clock | yes | yes | Out (CLOCK OUT on): 24 a quarter note while the drone plays and no clock comes in, sent as the sound is heard. In (CLOCK IN on): the tempo follows it, and its phase is held. CLOCK IN and CLOCK OUT are the MIDI page's MIDI CLOCK switches. `JD_MIDI_CLOCK=0` sends none; `JD_MIDI_FOLLOW=0` ignores it. |
| Start, Continue, Stop | yes | yes | Out: Start and Stop with the drone, unless START OUT is off (then the clock alone). In: they start, continue and stop it, unless START IN is off (then the clock's tempo only). |
| Active sensing | none | no | |
| All notes off (123), all sound off (120) | none | yes | Everything let go. |
| Reset | none | no | |

`JD_MIDI=0` in `launch.sh` turns MIDI off.

## Fixed CCs

| CC | Does |
|---|---|
| 1 (mod wheel) | Moves what MOD picks on the MIDI page: BOW (the default), TONE, SWELL, VIBRATO, PITCH, HARM, FOLD, BUZZ or OFF (aftertouch's TOUCH picks the same way) |
| 64 (sustain) | Held keys sound on after they're let go |
| 74 | TONE, darker to brighter |
| 91 | More or less reverb |
| 120, 123 | Everything let go |

## Settings by CC

The important voice, engine and play settings, on any channel. A CC's 0-127 spans the setting's whole range. A
voice's settings act on the voice of the channel the CC comes on (the MIDI page's channels), or on other channels
the selected string, as the buttons do: CC 2 chooses it. The defaults below use numbers MIDI leaves free, in
blocks: 14-29 the voice, 85-90 the engine, 102-112 the string's rings, 113-119 the play.

**MIDI learn** changes them: select any control on SOUND, COMPOSE, MIX or SONG, press START + B and move a knob.
The MIDI page lists the map: A learns an entry again, Y / X change its CC, R2 + Y forgets it. The map and the
page's settings are kept in `midi.cfg` in the app folder (they belong to the Brick, not to songs).

<!-- cc table -->
| CC | Setting | Range |
|---|---|---|
| 2 | STRING: the one the string CCs act on | 0-25 1, 26-50 2, 51-76 3, 77-101 4, 102-127 5 |
| 3 | VOICING | 0 to 1 |
| 9 | SUB | 0 to 100 |
| 14 | String NOTE | -24 to 24 |
| 15 | String FINE | -50 to 50 |
| 16 | String LEVEL | 0 to 100 |
| 17 | String BOW | 0 to 100 |
| 18 | String DECAY | 0 to 100 |
| 19 | String BRIGHT | 0 to 100 |
| 20 | String FOLD | 0 to 100 |
| 21 | String STIFF | 0 to 100 |
| 22 | String POS | 0 to 100 |
| 23 | String BUZZ | 0 to 100 |
| 24 | String HARM | 0 to 4 |
| 25 | String VOICE | 0 to 19 |
| 26 | String PAN | -50 to 50 |
| 27 | String ATTACK | 0 to 100 |
| 28 | String RELEASE | 0 to 100 |
| 29 | String ON | 0-63 muted, 64-127 on |
| 85 | ENGINE | 0 to 1 |
| 86 | EXCITER | 0 to 6 |
| 87 | BORE | 0 to 3 |
| 88 | SCALE | 0 to 16 |
| 89 | ROOT | 24 to 72 |
| 90 | BPM | 40 to 200 |
| 102 | String PLUCK | 0 to 1 |
| 103 | String P STEPS | 1 to 16 |
| 104 | String P PULSES | 1 to 16 |
| 105 | String P ROTATE | 0 to 15 |
| 106 | String BOW RING | 0 to 1 |
| 107 | String B STEPS | 1 to 16 |
| 108 | String B PULSES | 1 to 16 |
| 109 | String B ROTATE | 0 to 15 |
| 110 | String RATE | 1/32 to 2 BARS, shortest to longest (triplets, dotted and quintuple between) |
| 111 | String P CHANCE | 0 to 100 |
| 112 | String B CHANCE | 0 to 100 |
| 113 | PLAY MODE | 0 to 1 |
| 114 | CHANGES | 0 to 5 |
| 115 | DRIFT | 0 to 100 |
| 116 | GLIDE | 0 to 7 |
| 117 | CUTOFF | 0 to 100 |
| 118 | MOTION | 0 to 100 |
| 119 | MASTER | 0 to 100 |
<!-- end cc table -->
