#!/bin/sh
# Regenerate manifest/functions_ghidra.tsv with Ghidra's headless analyzer (a few minutes).
#   scripts/ghidra_export.sh /path/to/Patrician3.exe
set -e
cd "$(dirname "$0")/.."
EXE="${1:-$P3_EXE}"
[ -n "$EXE" ] || { echo "usage: scripts/ghidra_export.sh /path/to/Patrician3.exe" >&2; exit 2; }
HEADLESS="${GHIDRA_HEADLESS:-$(brew --prefix ghidra 2>/dev/null)/libexec/support/analyzeHeadless}"
mkdir -p .ghidra out
"$HEADLESS" .ghidra P3 -import "$EXE" -overwrite -scriptPath ghidra -postScript ExportFunctions.java out/functions_ghidra.raw.tsv > out/ghidra.log 2>&1
{
  echo "# Function starts of Patrician3.exe from Ghidra auto-analysis."
  echo "# Addresses, body sizes and Ghidra names only; no code or bytes. Regenerate: scripts/ghidra_export.sh"
  echo "# exe sha256 $(shasum -a 256 "$EXE" | cut -d' ' -f1)"
  cat out/functions_ghidra.raw.tsv
} > manifest/functions_ghidra.tsv
echo "ok: $(grep -vc '^#' manifest/functions_ghidra.tsv) functions -> manifest/functions_ghidra.tsv"
