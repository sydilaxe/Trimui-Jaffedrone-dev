#!/bin/sh
# Jaffedrone launcher for TrimUI OS (TrimUI Brick, Brick Hammer and Brick Pro).
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
# sample every 10 s while it plays (up to an hour). Written to sysinfo.txt.
{
    echo "== start $(date)"
    cat /etc/version 2>/dev/null; cat /usr/trimui/version 2>/dev/null
    echo "DEVICE=$DEVICE TRIMUI_MODEL=$TRIMUI_MODEL"
    uname -a
    grep -i "processor\|model name\|bogomips\|hardware\|features" /proc/cpuinfo
    echo "cores online: $(cat $CPU/online)"
    for f in scaling_governor scaling_cur_freq scaling_min_freq scaling_max_freq scaling_available_frequencies \
        scaling_available_governors; do
        echo "$f: $(cat $CPU/cpu0/cpufreq/$f 2>/dev/null)"
    done
    for z in /sys/class/thermal/thermal_zone*; do [ -d "$z" ] && echo "$z: $(cat $z/type 2>/dev/null) $(cat $z/temp 2>/dev/null)"; done
    echo "-- memory"; free; cat /proc/loadavg
    echo "-- sound cards"; cat /proc/asound/cards 2>/dev/null; ls /dev/snd 2>/dev/null
    echo "-- usb"; cat /sys/kernel/debug/usb/devices 2>/dev/null | grep -E "^[TPS]:" ; ls /sys/bus/usb/devices 2>/dev/null
    echo "-- input"; cat /proc/bus/input/devices 2>/dev/null | grep -E "^[NH]:"
    echo "-- processes"; ps
    echo "-- interrupts"; cat /proc/interrupts
} > sysinfo.txt 2>&1
touch /tmp/jaffedrone_running
(
    n=0
    while [ -f /tmp/jaffedrone_running ] && [ $n -lt 360 ]; do
        sleep 10
        n=$((n + 1))
        {
            echo "== sample $n at $(cut -d' ' -f1 /proc/uptime) s: load $(cat /proc/loadavg)," \
                "cpu $(cat $CPU/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) kHz," \
                "cores $(cat $CPU/online)"
            top -b -n 1 2>/dev/null | head -14
        } >> sysinfo.txt 2>&1
    done
) &

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
#   JD_ROTATE=180      turn the picture, for a firmware whose screen is turned
#   JD_JOYMAP=A=1,B=0  another firmware's button numbers
./jaffedrone > log.txt 2>&1

rm -f /tmp/jaffedrone_running
[ -n "$gov_was" ] && echo "$gov_was" > $CPU/cpu0/cpufreq/scaling_governor 2>/dev/null
[ -n "$min_was" ] && echo "$min_was" > $CPU/cpu0/cpufreq/scaling_min_freq 2>/dev/null
