# Jaffedrone MIDI implementation chart

Jaffedrone reads and sends MIDI over USB: plug a class-compliant keyboard, controller, sequencer or drum machine
into the TrimUI's USB-C port (through an OTG adapter or hub). Ports are found as they are plugged in. Everything below works
from any page.

## Summary

| Function | Transmitted | Recognized | Remarks |
|---|---|---|---|
| Basic channel | none | 1-16 (any) | Every message is heard on any channel. With `JD_MIDI_NOTES=CHANNELS`, notes on channels 1-5 play strings 1-5. |
| Mode | none | Omni on, poly | |
| Note number | none | 0-127 | Four notes at once, each sounding the voice selected on PLAY at the key's pitch, on voices of their own; a fifth key takes the voice of the one held longest. With `JD_MIDI_NOTES=CHANNELS`, a key tunes its string to itself (out of reach: the nearest octave). |
| Velocity: note on | none | yes | How hard the note is plucked, and the bow's swell. |
| Velocity: note off | none | no | |
| Aftertouch: key | none | no | |
| Aftertouch: channel | none | yes | How hard the held strings are bowed. |
| Pitch bend | none | yes | ±2 semitones. |
| Control change | none | yes | Fixed: 1, 64, 74, 91, 120, 123 (below). Settings: the CC table (below). |
| Program change | none | no | |
| System exclusive | yes | yes | Only the Brick link's position (`F0 7D 'T' 'L' ... F7`, 14 bytes, about 50 a second, to linked Bricks only); others pass by. |
| Song position | none | yes | Places the drone (not with START set to IGNORE). |
| Song select | none | no | |
| Clock | yes | yes | Out: 24 a quarter note while the drone plays and no clock comes in, sent as the sound is heard. In: the tempo follows it, and its phase is held. MIX's MIDI setting picks the way (AUTO, SEND, FOLLOW, OFF). `JD_MIDI_CLOCK=0` sends none; `JD_MIDI_FOLLOW=0` ignores it. |
| Start, Continue, Stop | yes | yes | Out with the drone; in, they start, continue and stop it, unless MIX's START is set to IGNORE. |
| Active sensing | none | no | |
| All notes off (123), all sound off (120) | none | yes | Everything let go. |
| Reset | none | no | |

`JD_MIDI=0` in `launch.sh` turns MIDI off.

## Fixed CCs

| CC | Does |
|---|---|
| 1 (mod wheel) | How hard the held strings are bowed |
| 64 (sustain) | Held keys sound on after they're let go |
| 74 | TONE, darker to brighter |
| 91 | More or less reverb |
| 120, 123 | Everything let go |

## Settings by CC

The important voice, engine and play settings, all on one channel (any channel is heard). A CC's 0-127 spans the
setting's whole range. The string settings act on the selected string, as the buttons do: CC 2 chooses it. The CC
numbers are ones MIDI leaves free, in blocks: 14-29 the voice, 85-90 the engine, 102-112 the string's rings,
113-119 the play.

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
| 110 | String RATE | 0 to 5 |
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
