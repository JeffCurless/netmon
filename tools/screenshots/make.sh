#!/usr/bin/bash
# MIT License with Commons Clause
#
# Copyright (c) 2026 Jeff Curless
#
# Required Notice: Copyright (c) 2026 Jeff Curless.
#
# This software is licensed under the MIT License, subject to the Commons Clause
# License Condition v1.0. You may use, copy, modify, and distribute this software,
# but you may not sell the software itself, offer it as a paid service, or use it
# in a product or service whose value derives substantially from the software
# without prior written permission from the copyright holder.

# make.sh — render NetMon's screens on the host into docs/images/*.png
#
# Builds tools/screenshots/shots.c (which includes the real apps/netmon.c)
# with the host gcc, runs it, and converts its PPM frames to PNG at 2x.
# Needs gcc and python3 with Pillow (pip install pillow).
#
# Usage: tools/screenshots/make.sh [output-dir]   (default docs/images)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
OUT="${1:-$ROOT/docs/images}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

DRV="$ROOT/picoOS/src/drivers/display.c"
# The driver's font table and drawing primitives, used as-is.
sed -n '/^static const uint8_t font8x8/,/^};/p' "$DRV" > "$WORK/font.inc"
sed -n '/^static void draw_pixel_impl/,/^int display_open/p' "$DRV" \
    | sed '$d' > "$WORK/draw.inc"

VERSION="$(tr -d '[:space:]' < "$ROOT/VERSION")"
gcc -std=gnu11 -O1 -w \
    -DPICOOS_DISPLAY_ENABLE -DPICOOS_DISPLAY_PACK2 \
    -DPICOOS_WIFI_ENABLE -DPICOOS_BT_ENABLE \
    -DNETMON_VERSION="\"$VERSION\"" \
    -DPICOOS_VERSION_MAJOR=0 -DPICOOS_VERSION_MINOR=5 -DPICOOS_VERSION_EDIT=2 \
    -I"$WORK" -I"$ROOT/apps" -I"$ROOT/picoOS/src" \
    "$ROOT/tools/screenshots/shots.c" "$ROOT/apps/oui.c" "$ROOT/apps/oui_data.c" \
    -o "$WORK/shots"

mkdir -p "$OUT"
"$WORK/shots" "$WORK"
python3 - "$WORK" "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image
src, dst = sys.argv[1], sys.argv[2]
for p in sorted(glob.glob(os.path.join(src, "*.ppm"))):
    im = Image.open(p)
    im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
    name = os.path.splitext(os.path.basename(p))[0] + ".png"
    im.save(os.path.join(dst, name), optimize=True)
    print(os.path.join(dst, name))
EOF
