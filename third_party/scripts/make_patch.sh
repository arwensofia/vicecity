#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Regenerate a local patch from edits made in third_party/pcsx2:
#   third_party/scripts/make_patch.sh 0001-name.patch pcsx2/GS/Renderers/Metal/GSMTLDeviceInfo.mm [...]
# Paths are relative to the PCSX2 root (third_party/pcsx2 == third_party/_upstream/pcsx2).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
name="$1"; shift
out="$ROOT/third_party/patches/pcsx2/$name"
mkdir -p "$(dirname "$out")"
: > "$out"
for f in "$@"; do
	diff -u --label "a/$f" --label "b/$f" "$ROOT/third_party/_upstream/pcsx2/$f" "$ROOT/third_party/pcsx2/$f" >> "$out" || true
done
echo "wrote $out"
