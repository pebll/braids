#!/usr/bin/env bash
#
# tray-icons.sh
#
# Copyright (c) 2026 braids contributors
#
# braids is distributed under the GNU AGPL v3 license, see LICENSE for details.
#
# Generates the tray icons in src-tauri/icons from doc/design/icons: the
# monochrome icons (no unread chats) and tray-unread-{1,2,3}.png, where one,
# two or all three strands are colored for 1, 2 and 3+ unread chats.
# Usage (from app/): scripts/tray-icons.sh

set -e

APP="$(cd "$(dirname "$0")/.." && pwd)"
SRC="${APP}/../doc/design/icons"
OUT="${APP}/src-tauri/icons"
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

render() {
  # 32x32 png via the tauri icon tool (resvg)
  (cd "${APP}" && npx tauri icon "$1" -o "${TMP}/out" > /dev/null 2>&1)
  cp "${TMP}/out/32x32.png" "$2"
  rm -rf "${TMP}/out"
}

render "${SRC}/braids-tray-black.svg" "${OUT}/tray-black.png"
render "${SRC}/braids-tray-white.svg" "${OUT}/tray-white.png"

for N in 1 2 3; do
  python3 - "${SRC}/braids-tray-white.svg" "${TMP}/tray-${N}.svg" "${N}" << 'PYEOF'
import sys
src, dst, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
colors = ['#E58A55', '#E9B373', '#D0663A']  # dark-logo copper, gold, deep copper
svg = open(src).read()
parts = svg.split('stroke="#fff"')
out = parts[0]
for i, part in enumerate(parts[1:]):
    out += f'stroke="{colors[i] if i < n else "#fff"}"' + part
open(dst, 'w').write(out)
PYEOF
  render "${TMP}/tray-${N}.svg" "${OUT}/tray-unread-${N}.png"
done

echo "tray icons written to ${OUT}"
