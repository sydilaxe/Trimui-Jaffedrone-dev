Jaffedrone test build for the TrimUI Brick, Brick Hammer and Brick Pro (TrimUI OS and MustardOS).

**Install on TrimUI OS:** unzip `Jaffedrone-brick.zip` (Brick, Brick Hammer) or `Jaffedrone-brickpro.zip` (Brick
Pro) at the root of the SD card (it contains `Apps/Jaffedrone/`), then open **Apps → Jaffedrone**.

**Install on MustardOS:** copy `Jaffedrone-mustardos-brick.muxapp` (Brick, Brick Hammer) or
`Jaffedrone-mustardos-brickpro.muxapp` (Brick Pro) into the `ARCHIVE` folder of the first SD card, install it
from **Applications → Archive Manager**, then open **Applications → Jaffedrone**.

**New: the important settings by MIDI CC**
- The voice, engine and play settings that matter most each have a MIDI CC, on any channel: CC 2 chooses the
  string, 14-29 set its voice (NOTE, LEVEL, BOW, DECAY, BRIGHT ... ON), 85-90 the engine (ENGINE, EXCITER,
  BORE, SCALE, ROOT, BPM), 102-112 the string's rings, 113-119 the play (PLAY MODE, CHANGES, DRIFT, GLIDE,
  CUTOFF, MOTION, MASTER). A CC acts as a press would, and is saved with the song.
- `docs/MIDI.md` is the MIDI implementation chart.

**Fix: the screen going black after the start-up screen**
- The picture is now sent to the screen opaque. Its pixels went out with their alpha (transparency) at 0,
  which TrimUI OS ignores but MustardOS's display shows, so on MustardOS nothing the app drew was visible:
  not the start-up screen, not the pages.
- After MustardOS's screen wake the app now makes its renderer again, and it rebuilds the picture whenever
  SDL reports it lost or drawing fails, so the pages show instead of a black screen. The log's `screen:`
  lines record the first frame, the wake, the first page, window events and every rebuild.

**New: a MIDI keyboard plays the selected voice, and chords across the others**
- The first key plays the voice selected on PLAY. Keys added while others are held take the next free voices,
  so a chord builds across them and overlapping keys rotate through them; with all five held, the oldest key
  gives up its voice. Muted voices are passed over.

**New: the Brick Pro's sticks reach further, and light up**
- **Full bowing:** the left stick now bows from a light touch to the engine's hardest (half again as hard as
  R1). Each stick learns how far it really goes, and a diagonal reaches both ends, so bowing while bending
  still reaches full.
- **Right stick:** up / down plays the harmonics (HARM; a wind's register, a struck voice's sung mode);
  left / right plays a SOUND control of your choice (BRIGHT at first). On SOUND, click the right stick on a
  row to put it on the stick; the choice is kept.
- **Lights:** the lights around the sticks glow as they are pushed, brighter the further they go (amber
  bowing, rose bending, green harmonics, gold the mapped control), and go back to how they were on leaving.
  On MustardOS, which starts with the lights turned down to nothing, they are turned up while Jaffedrone
  runs (`JD_LED_BRIGHTNESS`, 1 .. 60) and turned down again on leaving. `JD_LEDS=0` in `launch.sh` leaves
  them alone.

**New: MustardOS, and logs for tuning**
- Packages for MustardOS on the Brick, Brick Hammer and Brick Pro. The app reads MustardOS's controller, so
  every button (and the Brick Pro's sticks) works as on TrimUI OS.
- The screen no longer stays dark on a Brick Pro under MustardOS.
- The log names the firmware, board and build at start, flags the wrong build for the device, sums up the
  session's dropouts, load, clock and temperature at the end, and at DETAIL logs every button as the
  controller sends it. `sysinfo.txt` now covers MustardOS too.

**New on TrimUI**
- Ported from the Miyoo Mini Plus: the same instrument and the same sound, on SDL2, the pages scaled 1.6x to
  fill the 1024x768 screen.
- **Four cores:** the voices and the effects are made on two cores of their own, a block apart (the same
  sound, sample for sample); the screen and the system keep the other two. Half the Miyoo's audio delay
  (512-frame buffers).
- **USB MIDI:** keys play the strings at their own pitch, plucking by velocity and bowing while held; pitch
  bend, mod wheel, pressure, sustain, CC74 (tone) and CC91 (reverb). Clock, START and STOP in and out.
- **USB audio:** a USB sound card or DAC takes over the output while it is plugged in.
- **Brick Pro:** the left stick bows (harder the further it goes) and bends the selected string, the right
  stick plays the harmonics and a SOUND control; L3 plucks, R3 changes chord.

**From the Miyoo version — new in 0.13**
- **Mono + clock out**, as on HI5: on SONG, set **OUTPUT** to **MONO+CLK** and the sound plays in mono on one
  side of the headphone jack, with clock pulses on the other to lead a Pocket Operator, Volca or other gear.
  **CLOCK ON** picks the side, **PPQN** the pulses per quarter note (2 for PO/Volca, 24 for DIN) and
  **PULSE** the width. The pulses follow the beat and Wi-Fi sync, and stop with the drone.
- In mono + clock every pan and stereo spread plays from the centre; your settings come back with STEREO.
- Songs save the output (format JDR17); older songs load in STEREO. Loading a preset keeps the output settings.

**New in 0.12.2**
- Cleaner struck voices: a mallet striking a body that is already ringing no longer starts inside its
  surface (a violent spike that the limiter clipped flat: the crunch on hard strikes, L2 drops and rhythms
  on long-ringing gongs and bowls). The struck limiter now turns the voice down smoothly instead of
  clipping its waveform.

**New in 0.12.1**
- AIR is renamed **AURA**, so AIR means only a wind's breath noise.

**New in 0.12**
- **AURA**: voices that listen. On MIX, set a voice's **AURA** to **LISTEN** and it is never played: it
  sounds only from the others' sound reaching it through the air of a shared room, as a gong hums beside
  a flute or bowls sing when a reed plays. **SEND** voices play into the air, **OFF** voices stay out of
  it; **AMOUNT** (under AURA, left of the effects) sets how strongly the listeners respond. Each voice
  stands where its PAN puts it: further away is later and fainter. The sound only goes one way, players to
  listeners, so it can never feed back. Works in RINGS and LATCH, with delay, reverb and binaural after.
- The effects on MIX run in the order the sound passes through them: AURA, DELAY, REVERB, BINAURAL.
- Songs save AURA (format JDR16); older songs load with AURA off and every voice on SEND.

**New in 0.11.4**
- The shoulders play the moment they are pressed: **L1** plucks, **R1** bows, **L1 + R1** a harmonic,
  and **L2** drops a dulcimer mallet on the string: a bright hit, then the mallet bounces, each rebound
  lower and sooner, until it rests. On a wind voice **L2** is a flutter tongue; on a struck voice a hard
  mallet, bouncing. Held, L2 still shapes the ring with the d-pad; the quiet view moves to **START + L2**.
- Delay and reverb start off, like binaural: raise MIX and SEND on the MIX page.

**New in 0.11.3**
- No more crackles that never stop: the sound is now made a buffer ahead by its own thread. The Miyoo's
  driver asks for two buffers at once, which left the sound only half the time it seemed to have; once a
  pair came late the driver stayed out of step. Made ahead, the same sound has nearly twice the room.
- Buttons are read every few milliseconds instead of once a screen frame (up to 50 ms), so presses reach
  the sound sooner, more than making up the 21 ms the sound is made ahead.
- The launcher writes `sysinfo.txt`: Onion's settings, the processor and the audio driver, sampled while
  playing. `JD_BUFFER=512` in `launch.sh` halves the delay from a press to the sound.

**New in 0.11.2**
- Lighter struck voices with the same sound: their modes are worked out only when something changes, run
  four at a time on the Miyoo's vector unit, and modes that have died away are rested.
- The log now shows the processor's clock, temperature and cores, and how regularly the audio is called,
  to catch what causes crackles at a modest load.
- `make bench` ranks 32 voice mixtures at their worst.

**New in 0.11.1**
- More room for sound: the projection is gone, the screen runs below the audio and slows to 20 frames a
  second when the sound is working hard. (A heavy patch of struck voices could crackle before.)

**New in 0.11**
- Metal drones: RING from 95 up rings for up to ten minutes (DRONE), keeping its shimmer.
- Beyond the acoustic: STRETCH reaches much further, and three new bodies, GLASS, CHORD (tuned to the
  scale) and ALIEN (golden-ratio modes).
- Each struck body has its own limiter, so strikes on a long ring never pile up.

**New in 0.10**
- Struck voices: nine physically modelled bodies for any voice: GENDER, BONANG and GONG (gamelan, with
  ombak), BOWL, HANDPAN, STEELPAN, TONGUE, CYMBAL and DRUM. Struck by a mallet that bounces, or bowed by
  stick-slip friction until they sing. R2 + Y / X on VOICE jumps between strings, winds and struck voices.

**New in 0.9.1**
- Overblows and harmonics with both shoulders in either order: bowing with R1, add L1 (also in LATCH,
  where B held does the same).

**New in 0.9**
- Tap L1 to strike a string like a hammered dulcimer (a wind voice: a hard overblown attack); hold it
  briefly to pluck.
- LATCH: hold B for the next harmonic of a droning string; Y / X set each string's bow attack.

**New in 0.8.2**
- LATCH: a long RELEASE now dies away properly (the leveller no longer lifts a fading string back up).

**New in 0.8.1**
- Fixes for clicks and dropouts: the audio thread runs at real-time priority, the screen always pauses
  between frames and draws the projection a row at a time (much lighter), and changing a voice fades the
  old one out instead of cutting it. The log shows the screen's frame time and the audio priority.

**New in 0.8**
- LATCH play mode (START + A): every string is silent until you pluck (L1) or bow (R1) it, then drones
  until you play it again, and fades over its own RELEASE (0.05 to 20 s; up and down on PLAY).

**New in 0.7**
- A new wordmark, after a drawing and the Jaffe house, and a start-up screen on the house's roofline.
- Balinese gong kebyar tunings BEGBEG, SEDANG and TIRUS, and binaural MODE OMBAK: the gamelan's paired
  beating, for both ears and the speaker.
- Faster DSP (about 15% less work), and the single reed no longer jumps to its upper register at the attack.

**New in 0.6**
- Each voice can be a string or one of seven winds (VOICE row on the SOUND page).
- Louder undertones, blown wind attacks, binaural from 0.1 to 40 Hz with journeys, even loudness.

If you hear crackles, set `JD_BUFFER=1024` in `Apps/Jaffedrone/launch.sh`. The log is written to
`Apps/Jaffedrone/jaffedrone_log.txt` (START + SELECT cycles OFF / BASIC / DETAIL), and the launcher writes
a system report to `Apps/Jaffedrone/sysinfo.txt`.

The manual, `Apps/Jaffedrone/Jaffedrone-Manual.pdf`, shows every control on a drawing of the Brick.
