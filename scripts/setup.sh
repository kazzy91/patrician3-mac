#!/bin/sh
# Create the Python environment used by the recompiler (capstone, unicorn).
set -e
cd "$(dirname "$0")/.."
python3 -m venv .venv
.venv/bin/pip install -q -r requirements.txt
echo "ok: .venv ready"
