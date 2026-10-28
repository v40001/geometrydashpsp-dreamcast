#!/bin/bash
# dc/build.sh - build the Dreamcast binary
#
#   ./dc/build.sh            gd.elf
#   ./dc/build.sh clean
#
# Sources $KOS_BASE/environ.sh (default ~/dc/kos). Needs the sh-elf toolchain
# that comes with KallistiOS.
set -e

cd "$(dirname "$0")/.."

export KOS_BASE="${KOS_BASE:-$HOME/dc/kos}"
if [ ! -f "$KOS_BASE/environ.sh" ]; then
	echo "KOS_BASE=$KOS_BASE has no environ.sh: build KallistiOS first (see dc/README.md)" >&2
	exit 1
fi
. "$KOS_BASE/environ.sh"

make -f Makefile.dc "$@"
