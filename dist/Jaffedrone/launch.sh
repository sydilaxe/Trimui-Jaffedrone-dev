#!/bin/sh
# Jaffedrone launcher for OnionOS. Mirrors Onion's own GMU launcher: take the
# audio device from audioserver and use Onion's MI_AO/MI_GFX-backed SDL 1.2.
cd "$(dirname "$0")"

echo performance > /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor
[ -f /mnt/SDCARD/.tmp_update/script/stop_audioserver.sh ] && . /mnt/SDCARD/.tmp_update/script/stop_audioserver.sh

touch /tmp/stay_awake
unset LD_PRELOAD
export LD_LIBRARY_PATH="/mnt/SDCARD/.tmp_update/lib/parasyte:/lib:/config/lib:/mnt/SDCARD/miyoo/lib:/mnt/SDCARD/.tmp_update/lib:$LD_LIBRARY_PATH"

# A report of the system, to tune Jaffedrone for it: Onion's version and
# settings, the processor, what is running and the audio driver's state, and
# while it plays, a sample every 10 s (up to an hour). Written to sysinfo.txt.
{
    echo "== start $(date)"
    cat /mnt/SDCARD/.tmp_update/onionVersion/version.txt 2>/dev/null
    uname -a
    grep -i "processor\|model name\|bogomips\|hardware\|features" /proc/cpuinfo
    echo "cores online: $(cat /sys/devices/system/cpu/online)"
    for f in scaling_governor scaling_cur_freq scaling_min_freq scaling_max_freq scaling_available_frequencies \
        scaling_available_governors; do
        echo "$f: $(cat /sys/devices/system/cpu/cpu0/cpufreq/$f 2>/dev/null)"
    done
    for z in /sys/class/thermal/thermal_zone*; do [ -d "$z" ] && echo "$z: $(cat $z/type 2>/dev/null) $(cat $z/temp 2>/dev/null)"; done
    echo "-- onion config"; ls -la /mnt/SDCARD/.tmp_update/config/ 2>/dev/null
    echo "-- system.json"; cat /appconfigs/system.json 2>/dev/null
    echo "-- memory"; free; cat /proc/loadavg
    echo "-- processes"; ps
    echo "-- audio"; ls /proc/mi_modules/ 2>/dev/null; cat /proc/mi_modules/mi_ao/mi_ao0 2>/dev/null
    cat /proc/asound/cards 2>/dev/null
    echo "-- interrupts"; cat /proc/interrupts
} > sysinfo.txt 2>&1
(
    n=0
    while [ -f /tmp/stay_awake ] && [ $n -lt 360 ]; do
        sleep 10
        n=$((n + 1))
        {
            echo "== sample $n at $(cut -d' ' -f1 /proc/uptime) s: load $(cat /proc/loadavg)," \
                "cpu $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq 2>/dev/null) kHz," \
                "cores $(cat /sys/devices/system/cpu/online)"
            top -b -n 1 2>/dev/null | head -14
            cat /proc/mi_modules/mi_ao/mi_ao0 2>/dev/null | head -40
        } >> sysinfo.txt 2>&1
    done
) &

# Optional settings, before ./jaffedrone (for example JD_BUFFER=512 ./jaffedrone):
#   JD_BUFFER=512   smaller audio buffers: less delay from a press to the sound (1024 by default)
#   JD_AHEAD=0      make the sound in the driver's request, as before 0.11.3 (to compare)
#   JD_RATE=44100   a lower sample rate: about 8% less work
./jaffedrone > log.txt 2>&1

rm -f /tmp/stay_awake
