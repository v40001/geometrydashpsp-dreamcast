#!/bin/bash
# dc/tools/build_assets.sh - build the disc's romfs/
#
#   ./dc/tools/build_assets.sh [ps2_romfs]
#
# Turns the game's tex3ds lists (gfx/*.t3s) into Dreamcast sprite sheets
# (.dcs), and copies the levels, songs, effects and menu layouts into
# dc/disc/romfs/. The PS2 port's assets are the default source for the songs
# (4-bit IMA ADPCM .wav, which is what dc/src/mp3_dc.c plays) and levels.
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
PS2="${1:-${PS2_ROMFS:-/Users/admin/Downloads/Geometry Dash PS2 Port OptiJuegos/Version ELF/GDPS2/romfs}}"
OUT="$ROOT/dc/disc/romfs"
PY="${PYTHON:-python3}"

cd "$ROOT"

if [ ! -d "$PS2" ]; then
	echo "no PS2 romfs at $PS2 (pass it as the first argument)" >&2
	exit 1
fi

rm -rf "$ROOT/dc/disc"
mkdir -p "$OUT/gfx" "$OUT/main_levels" "$OUT/songs" "$OUT/sfx" "$OUT/menus"

echo "sheets:"
for t3s in $(find gfx -name '*.t3s' | sort); do
	name="$(basename "$t3s" .t3s)"
	"$PY" dc/tools/mkatlas.py "$t3s" "$OUT/gfx/$name.dcs"
done

echo "levels, songs, effects:"
cp "$PS2"/main_levels/*.gmd "$OUT/main_levels/"
cp "$PS2"/songs/*.wav "$OUT/songs/"
cp "$PS2"/sfx/*.wav "$OUT/sfx/"
# the AICA sample player takes at most 65534 samples: resample the long ones
"$PY" - "$OUT/sfx" <<'PYEOF'
import sys, glob, wave, audioop
for f in glob.glob(sys.argv[1] + '/*.wav'):
    w = wave.open(f); n, r, ch, sw = w.getnframes(), w.getframerate(), w.getnchannels(), w.getsampwidth()
    data = w.readframes(n); w.close()
    if n <= 65000:
        continue
    nr = int(r * 65000 / n)
    data, _ = audioop.ratecv(data, sw, ch, r, nr, None)
    o = wave.open(f, 'wb'); o.setnchannels(ch); o.setsampwidth(sw); o.setframerate(nr); o.writeframes(data); o.close()
PYEOF

echo "menus:"
cp romfs/menus/*.txt "$OUT/menus/"
cp psp/menus/*_psp.txt "$OUT/menus/"

echo "romfs: $(du -sh "$OUT" | cut -f1)"
