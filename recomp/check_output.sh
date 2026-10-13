#!/bin/sh
# Compile-check recompiled output with clang (arm64).
#   recomp/check_output.sh [recomp_out] [extra clang flags, e.g. -O2 -c]
# Default: -fsyntax-only over every file, in parallel.
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
out=${1:-$root/recomp_out}
[ $# -gt 0 ] && shift
flags=${*:--fsyntax-only}
jobs=$(sysctl -n hw.ncpu 2>/dev/null || echo 8)
objdir="$out/obj"
mkdir -p "$objdir"
ls "$out"/*.c | xargs -P "$jobs" -I{} sh -c '
	f="$1"; shift
	clang -std=c11 -arch arm64 -I"'"$root"'/runtime/include" -I"'"$root"'/recomp/include" \
		-ffp-contract=off -fno-math-errno -fno-strict-aliasing -Wall -Werror=implicit-function-declaration \
		-Wno-unused-label -Wno-unused-function -Wno-unused-variable \
		'"$flags"' -o "'"$objdir"'/$(basename "$f" .c).o" "$f" || echo "FAILED: $f"
' sh {}
