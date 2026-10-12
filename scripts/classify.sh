#!/usr/bin/env bash
# Run ClassifyFunctions over the existing Ghidra project (no re-analysis), then
# cluster functions into modules. Run scripts/setup.sh first.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
GHIDRA_HOME="${GHIDRA_HOME:-/opt/homebrew/opt/ghidra/libexec}"
export JAVA_HOME="${JAVA_HOME:-$(brew --prefix openjdk@21 2>/dev/null)/libexec/openjdk.jdk/Contents/Home}"

"$GHIDRA_HOME/support/analyzeHeadless" "$ROOT/ghidra_project" vicecity \
  -process SLUS_205.52 -noanalysis -readOnly \
  -scriptPath "$ROOT/tools/ghidra" \
  -postScript ClassifyFunctions.java "$ROOT/out" > "$ROOT/out/classify.log" 2>&1
grep "ClassifyFunctions:" "$ROOT/out/classify.log"

python3 -I "$ROOT/tools/modules.py" "$ROOT/out"
