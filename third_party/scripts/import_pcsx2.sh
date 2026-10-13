#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Re-import the subset of PCSX2 used by vicecity (GS + Metal renderer, VU0/VU1 interpreters,
# VIF/GIF units and their support code + bundled third-party libraries) from an upstream
# checkout into third_party/pcsx2, then apply our local patches.
#
#   git clone https://github.com/PCSX2/pcsx2 third_party/_upstream/pcsx2
#   git -C third_party/_upstream/pcsx2 checkout "$(cat third_party/pcsx2/UPSTREAM_COMMIT)"
#   third_party/scripts/import_pcsx2.sh [manifest]
#
# The manifest (default third_party/pcsx2_files.txt) lists every imported file, relative to
# the PCSX2 root. It is generated from the compiler's dependency output by
# third_party/scripts/update_manifest.sh, so it contains exactly the sources we compile and
# the headers they include. Files are copied unmodified; every local change lives in
# third_party/patches/pcsx2/*.patch and is listed in third_party/README.md.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SRC="$ROOT/third_party/_upstream/pcsx2"
DST="$ROOT/third_party/pcsx2"
PATCHES="$ROOT/third_party/patches/pcsx2"
MANIFEST="${1:-$ROOT/third_party/pcsx2_files.txt}"

[[ -d "$SRC/.git" ]] || { echo "missing upstream checkout: $SRC" >&2; exit 1; }
[[ -f "$MANIFEST" ]] || { echo "missing manifest: $MANIFEST" >&2; exit 1; }

rm -rf "$DST"
mkdir -p "$DST"

n=0
while IFS= read -r f; do
	[[ -z "$f" || "$f" == \#* ]] && continue
	mkdir -p "$DST/$(dirname "$f")"
	cp -p "$SRC/$f" "$DST/$f"
	n=$((n + 1))
done < "$MANIFEST"

if compgen -G "$PATCHES/*.patch" > /dev/null; then
	for p in "$PATCHES"/*.patch; do
		echo "applying $(basename "$p")"
		patch -s -p1 -d "$DST" < "$p"
	done
fi

git -C "$SRC" rev-parse HEAD > "$DST/UPSTREAM_COMMIT"
echo "imported $n files from PCSX2 $(cat "$DST/UPSTREAM_COMMIT")"
