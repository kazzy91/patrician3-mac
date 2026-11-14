#!/bin/sh
# Translate the game's code to C and compile it for arm64: Patrician3.exe -> out/build/p3/, and every game DLL that
# has a manifest (manifest/<module>_functions.tsv) -> out/build/<module>/.
#   scripts/build.sh "/path/to/Patrician III"      (the folder, or the path of Patrician3.exe)
set -e
cd "$(dirname "$0")/.."
GAME="${1:-$P3_GAME}"
[ -n "$GAME" ] || { echo "usage: scripts/build.sh /path/to/Patrician III" >&2; exit 2; }
[ -f "$GAME" ] && GAME=$(dirname "$GAME")
[ -x .venv/bin/python ] || scripts/setup.sh
# case-insensitive file lookup in the game folder
find_file() { find "$GAME" -maxdepth 1 -iname "$1" | head -1; }
.venv/bin/python recomp/p3_build.py --exe "$(find_file Patrician3.exe)" --module p3
for f in manifest/*_functions.tsv; do
    [ -e "$f" ] || continue
    m=$(basename "$f" _functions.tsv)
    base=$(awk -v m="$m" '$1 == m { print $2 }' manifest/bases.tsv)
    .venv/bin/python recomp/p3_build.py --exe "$(find_file "$m.dll")" --module "$m" ${base:+--base $base}
done
