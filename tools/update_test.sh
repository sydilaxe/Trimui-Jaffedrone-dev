#!/bin/sh
# The self-update end to end, without the internet (make update-test; CI runs it). As in
# Columbia Street.
#
# tools/fake_github.py serves a v9.9.9 release the way GitHub does: the API's JSON, a
# ~1000-character signed redirect, then the MustardOS package (.muxapp, a zip).
# build/jd_update installs it over an older app folder, as the app does when you press
# A. First, the same release with a damaged zip must fail and leave the folder as it was.
set -eu

T=build/update-test
PORT=${PORT:-8767}
rm -rf $T
mkdir -p "$T/rel/Jaffedrone/glyph" $T/bad $T/app

# the release: launch.sh, mux_launch.sh and friends, a binary, the manual, the glyph in its
# folder, and a song, preset and MIDI map that must never reach the user's folder
R="$T/rel/Jaffedrone"
cp dist/Jaffedrone/launch.sh dist/Jaffedrone/cacert.pem dist/mustardos/mux_launch.sh "$R/"
cp dist/mustardos/glyph/jaffedrone.svg "$R/glyph/"
cp build/jd_update "$R/jaffedrone"
head -c 300000 /dev/urandom > "$R/Jaffedrone-Manual.pdf"
echo theirs > "$R/jaffedrone.sav"
echo theirs > "$R/preset_01.jds"
echo theirs > "$R/midi.cfg"
Z=Jaffedrone-mustardos-brick.muxapp
(cd $T/rel && zip -qr $Z "Jaffedrone")

# the device's folder: an older version, the user's song, preset, MIDI map and log, and a
# manual that's already the same as the release's (dated 2001, so we can tell it's left alone)
A=$T/app
cp dist/Jaffedrone/cacert.pem $A/
echo old > $A/mux_launch.sh
echo old > $A/jaffedrone
echo old > $A/launch.sh
echo mine > $A/jaffedrone.sav
echo mine > $A/preset_01.jds
echo mine > $A/midi.cfg
echo mine > $A/jaffedrone_log.txt
cp "$R/Jaffedrone-Manual.pdf" $A/
touch -t 200101010000 $A/Jaffedrone-Manual.pdf

# the same release with its zipped binary damaged (the CRC check must catch it)
cp $T/rel/$Z $T/bad/
off=$(python3 -c '
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); i = z.getinfo("Jaffedrone/jaffedrone")
print(i.header_offset + 30 + len(i.filename) + len(i.extra) + i.compress_size // 2)' $T/bad/$Z)
printf 'XXXXXXXX' | dd of=$T/bad/$Z bs=1 seek="$off" conv=notrunc 2>/dev/null

python3 tools/fake_github.py $T/rel --port $PORT --tag v9.9.9 --asset $Z &
good=$!
python3 tools/fake_github.py $T/bad --port $((PORT + 1)) --tag v9.9.9 --asset $Z &
bad=$!
trap 'kill $good $bad 2>/dev/null || true' EXIT
for i in $(seq 1 50); do
    curl -s -o /dev/null http://127.0.0.1:$PORT/latest.json && curl -s -o /dev/null http://127.0.0.1:$((PORT + 1))/latest.json && break
    sleep 0.1
done

fail() { echo "update test: $*" >&2; exit 1; }

cp -r $A $T/before
if JD_UPDATE_API=http://127.0.0.1:$((PORT + 1))/latest.json build/jd_update $A; then
    fail "a damaged zip was installed"
fi
diff -r $T/before $A >/dev/null || fail "a failed update changed the app folder"
echo "update test: a damaged zip is refused and changes nothing"

JD_UPDATE_API=http://127.0.0.1:$PORT/latest.json build/jd_update $A
cmp -s $A/jaffedrone "$R/jaffedrone" || fail "the binary wasn't replaced"
test -x $A/jaffedrone || fail "the binary isn't executable"
cmp -s $A/launch.sh "$R/launch.sh" || fail "launch.sh wasn't replaced"
cmp -s $A/mux_launch.sh "$R/mux_launch.sh" || fail "mux_launch.sh wasn't replaced"
test -x $A/mux_launch.sh || fail "mux_launch.sh isn't executable"
cmp -s $A/glyph/jaffedrone.svg "$R/glyph/jaffedrone.svg" || fail "the glyph's folder wasn't made"
for f in jaffedrone.sav preset_01.jds midi.cfg jaffedrone_log.txt; do
    test "$(cat $A/$f)" = mine || fail "the user's $f was overwritten"
done
test "$(date -r $A/Jaffedrone-Manual.pdf +%Y)" = 2001 || fail "an unchanged file was rewritten"
test -z "$(find $A -name '*.new')" || fail ".new files were left behind"
echo "update test: installed v9.9.9 (the .muxapp) over an older version, kept the song, preset, MIDI map and log, skipped the unchanged manual"
