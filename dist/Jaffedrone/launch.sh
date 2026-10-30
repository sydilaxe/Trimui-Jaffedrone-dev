#!/bin/sh
# Jaffedrone launcher for the TrimUI Brick, Brick Hammer and Brick Pro: TrimUI OS runs it
# directly, MustardOS through mux_launch.sh.
# The sound is made on cores 2 and 3, the screen and the system keep 0 and 1:
# all four stay online, at full speed, while Jaffedrone plays.
cd "$(dirname "$0")"

CPU=/sys/devices/system/cpu
gov_was=$(cat $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null)
min_was=$(cat $CPU/cpu0/cpufreq/scaling_min_freq 2>/dev/null)
for c in 0 1 2 3; do
    [ -f $CPU/cpu$c/online ] && echo 1 > $CPU/cpu$c/online 2>/dev/null
done
echo performance > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null

export LD_LIBRARY_PATH="/usr/trimui/lib:/usr/lib:$LD_LIBRARY_PATH"

# A report of the system, to tune Jaffedrone for it: the firmware, the
# processor, the audio and USB devices and what is running at start, then a
# sample every 10 s while it plays (up to an hour). Written to sysinfo.txt
# by sysinfo.sh (JD_SYSINFO=0 turns it off).
[ "${JD_SYSINFO:-1}" != 0 ] && ./sysinfo.sh start jaffedrone

# Optional settings, before ./jaffedrone (for example JD_BUFFER=256 ./jaffedrone):
#   JD_BUFFER=256      smaller audio buffers: less delay from a press to the sound (512 by default)
#   JD_RATE=44100      a lower sample rate
#   JD_SPLIT=0         make all the sound on one core, as on the Miyoo
#   JD_AUDIO=internal  always the speaker and headphones (default: a USB audio device when one is plugged in)
#   JD_AUDIO=<name>    the output device whose name contains <name> (see the log)
#   JD_MIDI=0          no USB MIDI
#   JD_MIDI_NOTES=CHANNELS   MIDI channels 1-5 play strings 1-5 (default: notes spread over the strings)
#   JD_MIDI_CLOCK=0    send no MIDI clock;  JD_MIDI_FOLLOW=0  ignore MIDI clock coming in
#   JD_BEND=7          Brick Pro: the left stick bends up to 7 semitones (2 by default)
#   JD_STICK_REACH=80  Brick Pro: take a stick to reach 80% of the way until it shows it goes further (60)
#   JD_LEDS=0          Brick Pro: leave the lights around the sticks alone
#   JD_LED_BRIGHTNESS=20  Brick Pro: the sticks' lights at 20 of 60 (default: as the system has them,
#                      or 60 when that is 0, as MustardOS leaves them)
#   JD_ROTATE=180      turn the picture, for a firmware whose screen is turned
#   JD_SCREEN_REBUILD=0  don't make the renderer again after the MustardOS screen wake
#   JD_JOYMAP=A=1,B=0  another firmware's button numbers (also L2, R2, L3, R3)
#   JD_LOG=2           log.txt's performance lines every second (1: every 10 s, the default; 0: off)
# The log, jaffedrone_log.txt, is set from the app: START + SELECT cycles OFF, BASIC and DETAIL;
# DETAIL adds a status line every second and every button press as the controller sends it.
./jaffedrone > log.txt 2>&1

./sysinfo.sh stop jaffedrone
[ -n "$gov_was" ] && echo "$gov_was" > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null
[ -n "$min_was" ] && echo "$min_was" > $CPU/cpu0/cpufreq/scaling_min_freq 2>/dev/null
