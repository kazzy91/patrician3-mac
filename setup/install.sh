#!/bin/sh
# Setup's build: checks the game files, translates and compiles the game on this Mac, makes "Patrician III.app" and
# puts it in Applications. Runs from the Setup app's payload (a copy of this repository's recomp/, runtime/,
# manifest/ and scripts/, plus capstone in pylib/).
#   install.sh "/path/to/Patrician III"
# Lines starting with @ are for the Setup window: "@step N text", "@done path", "@error text".
set -e
GAME="$1"
PAYLOAD=$(cd "$(dirname "$0")" && pwd)
WORK="$HOME/Library/Caches/patrician3-mac/setup"
fail() { echo "@error $*"; exit 1; }
step() { echo "@step $*"; echo "== $2" | sed "s/^== [0-9]* /== /"; }

step 1 "Checking your game files"
[ "$(uname -m)" = arm64 ] || fail "This Mac doesn't have Apple silicon. The port runs on M-series Macs only."
xcrun --find clang >/dev/null 2>&1 || fail "Apple's Command Line Tools are missing. Install them and try again."
[ -d "$GAME" ] || fail "That folder doesn't exist."
find_file() { find "$GAME" -maxdepth 1 -iname "$1" | head -1; }
while IFS="	" read -r file sum; do
    case "$file" in ''|'#'*) continue ;; esac
    f=$(find_file "$file")
    [ -n "$f" ] || fail "$file is missing from the folder. Choose the folder Patrician III is installed in."
    [ "$(shasum -a 256 "$f" | cut -d' ' -f1)" = "$sum" ] ||
        fail "$file is a different version from the one this port supports (Patrician III, English, retail CD)."
done < "$PAYLOAD/manifest/hashes.tsv"
free=$(df -k "$HOME" | awk 'NR == 2 { print $4 }')
[ "$free" -gt 4000000 ] || fail "Setup needs about 4 GB of free space."

step 2 "Preparing"
rm -rf "$WORK"
mkdir -p "$WORK"
cp -R "$PAYLOAD/recomp" "$PAYLOAD/runtime" "$PAYLOAD/manifest" "$PAYLOAD/scripts" "$WORK/"
cp "$PAYLOAD/VERSION" "$WORK/"
cd "$WORK"
export P3_PYTHON=/usr/bin/python3 PYTHONPATH="$PAYLOAD/pylib"

step 3 "Translating and compiling the game (this takes a few minutes)"
scripts/build.sh "$GAME"

step 4 "Linking"
runtime/build.sh

step 5 "Making the app"
scripts/app.sh "$GAME" "$WORK"

step 6 "Installing"
DEST=/Applications
[ -w "$DEST" ] || { DEST="$HOME/Applications"; mkdir -p "$DEST"; }
rm -rf "$DEST/Patrician III.app"
mv "$WORK/Patrician III.app" "$DEST/"
cd "$HOME"
rm -rf "$WORK"
echo "@done $DEST/Patrician III.app"
