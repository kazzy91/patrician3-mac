#!/bin/sh
# Regenerate a function manifest with Ghidra's headless analyzer (a few minutes).
#   scripts/ghidra_export.sh /path/to/Patrician3.exe              -> manifest/functions_ghidra.tsv
#   scripts/ghidra_export.sh /path/to/archiver.dll archiver        -> manifest/archiver_functions.tsv
set -e
cd "$(dirname "$0")/.."
BIN="${1:-$P3_EXE}"
[ -n "$BIN" ] || { echo "usage: scripts/ghidra_export.sh /path/to/module [name]" >&2; exit 2; }
NAME="$2"
OUT=manifest/functions_ghidra.tsv; [ -n "$NAME" ] && OUT=manifest/${NAME}_functions.tsv
HEADLESS="${GHIDRA_HEADLESS:-$(brew --prefix ghidra 2>/dev/null)/libexec/support/analyzeHeadless}"
mkdir -p out/ghidra_project
"$HEADLESS" out/ghidra_project P3 -import "$BIN" -overwrite -scriptPath ghidra -postScript ExportFunctions.java out/functions.raw.tsv > out/ghidra.log 2>&1
{
  echo "# Function starts of $(basename "$BIN") from Ghidra auto-analysis."
  echo "# Addresses, body sizes and Ghidra names only; no code or bytes. Regenerate: scripts/ghidra_export.sh"
  echo "# sha256 $(shasum -a 256 "$BIN" | cut -d' ' -f1)"
  cat out/functions.raw.tsv
} > "$OUT"
echo "ok: $(grep -vc '^#' "$OUT") functions -> $OUT"
