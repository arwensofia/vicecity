#!/usr/bin/env bash
# Extract the main executable from your own Vice City (USA) PS2 disc image
# and run Ghidra headless analysis on it.
#
#   scripts/setup.sh "/path/to/Grand Theft Auto - Vice City (USA) (v1.40).iso"
set -euo pipefail

ISO="${1:?usage: setup.sh <path-to-iso>}"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHIDRA_HOME="${GHIDRA_HOME:-/opt/homebrew/opt/ghidra/libexec}"
export JAVA_HOME="${JAVA_HOME:-$(brew --prefix openjdk@21 2>/dev/null)/libexec/openjdk.jdk/Contents/Home}"
ELF_NAME="SLUS_205.52"
EXPECTED_SHA1="${EXPECTED_SHA1:-}"

mkdir -p "$ROOT/game" "$ROOT/out" "$ROOT/ghidra_project"

if [[ ! -f "$ROOT/game/$ELF_NAME" ]]; then
  MNT="$(mktemp -d)"
  hdiutil attach -readonly -nobrowse -mountpoint "$MNT" "$ISO" >/dev/null
  trap 'hdiutil detach "$MNT" >/dev/null 2>&1 || true' EXIT
  grep -q "$ELF_NAME" "$MNT/SYSTEM.CNF" || { echo "Not the USA v1.40 disc (SYSTEM.CNF mismatch)"; exit 1; }
  cp "$MNT/$ELF_NAME" "$ROOT/game/"
  echo "Extracted $ELF_NAME"
fi

SHA1="$(shasum -a 1 "$ROOT/game/$ELF_NAME" | cut -d' ' -f1)"
echo "$ELF_NAME sha1: $SHA1"
if [[ -n "$EXPECTED_SHA1" && "$SHA1" != "$EXPECTED_SHA1" ]]; then
  echo "SHA1 mismatch: expected $EXPECTED_SHA1"; exit 1
fi

"$GHIDRA_HOME/support/analyzeHeadless" "$ROOT/ghidra_project" vicecity \
  -import "$ROOT/game/$ELF_NAME" -overwrite \
  -processor "MIPS:LE:64:64-32addr" \
  -scriptPath "$ROOT/tools/ghidra" \
  -postScript NameFromAsserts.java \
  -postScript ExportFunctions.java "$ROOT/out/functions.csv"
