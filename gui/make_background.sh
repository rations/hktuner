#!/usr/bin/env bash
# One-shot: downscale the 1536x1024 bezel master to the shipped window-size
# background. Run on the dev host; commit the output (ImageMagick is never a
# build dependency).
set -euo pipefail
cd "$(dirname "$0")"
. ./geometry.sh

magick src/blank-screen-background.png -resize "${WIN_W}x${WIN_H}" -depth 8 \
    PNG32:background.png
identify background.png
