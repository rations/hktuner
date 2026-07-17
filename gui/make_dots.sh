#!/usr/bin/env bash
# One-shot: cents-strip dot sprites (off/on) and the tall marker lozenge,
# rendered at 2x and downscaled. Run on the dev host; commit the outputs.
set -euo pipefail
cd "$(dirname "$0")"
. ./geometry.sh

D=$((DOT_SPR * 2))
R=$((D / 4))
C=$((D / 2))
M_W=$((MARKER_W * 2))
M_H=$((MARKER_H * 2))

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Lit dot: phosphor core + bloom.
magick -size "${D}x${D}" xc:black -fill white \
    -draw "circle $C,$C $C,$((C - R))" "$tmp/dotmask.png"
magick -size "${D}x${D}" "xc:${PHOSPHOR_CORE}" \
    "$tmp/dotmask.png" -alpha off -compose CopyOpacity -composite "$tmp/core.png"
magick -size "${D}x${D}" "xc:${PHOSPHOR_BLOOM}" \
    \( "$tmp/dotmask.png" -blur 0x4 \) -alpha off -compose CopyOpacity -composite \
    -channel A -evaluate multiply 0.6 +channel "$tmp/bloom.png"
magick "$tmp/bloom.png" "$tmp/core.png" -compose over -composite \
    -resize 50% -depth 8 PNG32:dot_on.png

# Off dot: faint gray.
magick -size "${D}x${D}" "xc:#3a4650" \
    "$tmp/dotmask.png" -alpha off -compose CopyOpacity -composite \
    -channel A -evaluate multiply 0.7 +channel \
    -resize 50% -depth 8 PNG32:dot_off.png

# Marker: white-hot vertical lozenge with a strong bloom (the "needle").
magick -size "${M_W}x${M_H}" xc:black -fill white \
    -draw "roundrectangle $((M_W / 2 - 5)),4 $((M_W / 2 + 5)),$((M_H - 5)) 5,5" \
    "$tmp/markmask.png"
magick -size "${M_W}x${M_H}" xc:white \
    "$tmp/markmask.png" -alpha off -compose CopyOpacity -composite "$tmp/markcore.png"
magick -size "${M_W}x${M_H}" "xc:${PHOSPHOR_CORE}" \
    \( "$tmp/markmask.png" -blur 0x7 \) -alpha off -compose CopyOpacity -composite \
    -channel A -evaluate multiply 0.8 +channel "$tmp/markbloom.png"
magick "$tmp/markbloom.png" "$tmp/markcore.png" -compose over -composite \
    -resize 50% -depth 8 PNG32:dot_marker.png

# In-tune marker: the same lozenge in green, shown when the reading is
# within the in-tune window and the marker sits over the center dot. The
# core is the saturated LED green (a pale core reads as white at this size)
# with a soft brighter center screened on top.
magick -size "${M_W}x${M_H}" xc:black -fill '#b0ffb0' \
    -draw "ellipse $((M_W / 2)),$((M_H / 2)) 4,$((M_H / 4)) 0,360" \
    -blur 0x5 "$tmp/markhot_g.png"
# Screen on an opaque canvas FIRST, then cut the mask alpha — screening an
# opaque layer onto an alpha'd one turns the whole sprite opaque.
magick -size "${M_W}x${M_H}" "xc:${LED_GREEN}" "$tmp/markhot_g.png" \
    -compose screen -composite \
    "$tmp/markmask.png" -alpha off -compose CopyOpacity -composite \
    "$tmp/marklit_g.png"
magick -size "${M_W}x${M_H}" "xc:${LED_GREEN}" \
    \( "$tmp/markmask.png" -blur 0x7 \) -alpha off -compose CopyOpacity -composite \
    -channel A -evaluate multiply 0.8 +channel "$tmp/markbloom_g.png"
magick "$tmp/markbloom_g.png" "$tmp/marklit_g.png" -compose over -composite \
    -resize 50% -depth 8 PNG32:dot_marker_green.png

identify dot_off.png dot_on.png dot_marker.png dot_marker_green.png