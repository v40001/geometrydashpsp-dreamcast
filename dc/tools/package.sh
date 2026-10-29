#!/bin/bash
# dc/tools/package.sh - the self-boot disc
#
#   ./dc/tools/package.sh
#
# gd.elf -> 1ST_READ.BIN (scrambled), dc/disc/IP.BIN + romfs -> gd.iso.
# Needs `scramble` and `mkisofs` (KOS's utils/scramble, cdrtools' mkisofs).
set -e

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

KOS_BASE="${KOS_BASE:-$HOME/dc/kos}"
SCRAMBLE="${SCRAMBLE:-$KOS_BASE/utils/scramble/scramble}"
MAKEIP="${MAKEIP:-$KOS_BASE/utils/makeip/src/makeip}"
MKISOFS="${MKISOFS:-mkisofs}"

if [ ! -f gd.elf ]; then
	echo "build first: ./dc/build.sh" >&2
	exit 1
fi
if [ ! -d dc/disc/romfs ]; then
	echo "assets first: ./dc/tools/build_assets.sh" >&2
	exit 1
fi

"${KOS_OBJCOPY:-sh-elf-objcopy}" -O binary gd.elf 1ST_READ.BIN
"$SCRAMBLE" 1ST_READ.BIN 1ST_READ.BIN.scr
mv 1ST_READ.BIN.scr 1ST_READ.BIN

if [ ! -f dc/disc/IP.BIN ]; then
	if [ -x "$MAKEIP" ]; then
		"$MAKEIP" -f -g "Geometry Dash" -c "OptiJuegos DC" -d 20261026 dc/disc/IP.BIN
	else
		echo "no dc/disc/IP.BIN and no makeip at $MAKEIP" >&2
		exit 1
	fi
fi

# Real GD-ROM bootstrap needs the track TOC inside IP.BIN: "TOC1" at 0x100,
# then 97 entries of (start LBA + 150) and type (0x41 data). Track 3 sits at
# LBA 45000; emulators skip this check but GD-ROM drives/ODEs do not.
python3 - dc/disc/IP.BIN <<'PY'
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
d[0x100:0x104] = b'TOC1'
d[0x104:0x288] = b'\xff' * (97 * 4)
d[0x104:0x108] = bytes((0x1e, 0xb0, 0x00, 0x41))  # 45150, data
open(p, 'wb').write(d)
PY

cp 1ST_READ.BIN dc/disc/
"$MKISOFS" -quiet -G dc/disc/IP.BIN -R -J -V GDROM -o gd.iso dc/disc/

# Flycast (and the other emulators) take a one-track CUE pointing at the ISO;
# a plain .iso is not a disc format they know.
cat > gd.cue <<CUE
FILE "gd.iso" BINARY
  TRACK 01 MODE1/2048
    INDEX 01 00:00:00
CUE

echo "gd.cue / gd.iso: $(du -h gd.iso | cut -f1)"

# GD-ROM image for GDEMU / real Dreamcast optical drive emulators (and
# Flycast): a small track 1, a silent audio track 2 and the game in the
# high-density area (track 3, from sector 45000).
GDI=gdi
rm -rf "$GDI" && mkdir -p "$GDI"
# a GD-ROM boots an unscrambled binary
"${KOS_OBJCOPY:-sh-elf-objcopy}" -O binary gd.elf dc/disc/1ST_READ.BIN
"$MKISOFS" -quiet -C 0,45000 -G dc/disc/IP.BIN -R -J -V GDROM -l -o "$GDI/track03.iso" dc/disc/
mkdir -p "$GDI/.t1"
printf 'This disc is for use with the Sega Dreamcast only.\n' > "$GDI/.t1/WARNING.TXT"
"$MKISOFS" -quiet -V GDMUSIC -o "$GDI/track01.iso" "$GDI/.t1"
rm -rf "$GDI/.t1"
dd if=/dev/zero of="$GDI/track02.raw" bs=2352 count=300 2>/dev/null
cat > "$GDI/disc.gdi" <<GD
3
1 0 4 2048 track01.iso 0
2 450 0 2352 track02.raw 0
3 45000 4 2048 track03.iso 0
GD
echo "gdi/disc.gdi listo"
