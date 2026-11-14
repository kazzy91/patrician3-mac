#!/bin/sh
# Translate Patrician3.exe to C and compile it for arm64 -> out/build/p3.a and out/build/report.txt.
#   scripts/build.sh /path/to/Patrician3.exe
set -e
cd "$(dirname "$0")/.."
EXE="${1:-$P3_EXE}"
[ -n "$EXE" ] || { echo "usage: scripts/build.sh /path/to/Patrician3.exe" >&2; exit 2; }
[ -x .venv/bin/python ] || scripts/setup.sh
.venv/bin/python recomp/p3_build.py --exe "$EXE"
