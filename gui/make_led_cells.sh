#!/usr/bin/env bash
# One-shot: LED bar cell sprites (off/green/amber/red), rendered at 2x and
# downscaled for crispness. Padded canvas so the bloom is part of the sprite
# and every state registers pixel-perfect. Run on the dev host; commit the
# outputs.
set -euo pipefail
cd "$(dirname "$0")"
. ./geometry.sh

S_W=$((LED_W * 2))
S_H=$((LED_H * 2))
C_W=$((LED_CORE_W * 2))
C_H=$((LED_CORE_H * 2))
X0=$(((S_W - C_W) / 2))
Y0=$(((S_H - C_H) / 2))
X1=$((X0 + C_W - 1))
Y1=$((Y0 + C_H - 1))

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# Rounded-rect cell mask (white on black), shared by all states.
magick -size "${S_W}x${S_H}" xc:black -fill white \
    -draw "roundrectangle $X0,$Y0 $X1,$Y1 6,6" "$tmp/mask.png"

# Soft wide mask for the bloom halo.
magick "$tmp/mask.png" -blur 0x8 "$tmp/halo.png"

make_lit() { # color outfile
    local color="$1" out="$2"
    # Hot core: blurred white ellipse screened onto the gradient body.
    magick -size "${S_W}x${S_H}" xc:black -fill white \
        -draw "ellipse $((S_W / 2)),$((Y0 + C_H / 3)) $((C_W / 3)),$((C_H / 4)) 0,360" \
        -blur 0x6 "$tmp/hot.png"
    # Body: vertical gradient of the color, slightly darker at the bottom.
    # Screen the hot core while still opaque, THEN cut the cell mask alpha —
    # screening an opaque layer onto an alpha'd one turns the sprite opaque.
    magick -size "${S_W}x${S_H}" "gradient:${color}-black" \
        -level 0%,140% \
        "$tmp/hot.png" -compose screen -composite \
        "$tmp/mask.png" -alpha off -compose CopyOpacity -composite "$tmp/lit.png"
    # Bloom halo underneath, tinted with the LED color.
    magick -size "${S_W}x${S_H}" "xc:${color}" \
        "$tmp/halo.png" -alpha off -compose CopyOpacity -composite \
        -channel A -evaluate multiply 0.55 +channel "$tmp/bloom.png"
    magick "$tmp/bloom.png" "$tmp/lit.png" -compose over -composite \
        -resize 50% -depth 8 "PNG32:$out"
}

make_lit "$LED_GREEN" led_green.png
make_lit "$LED_AMBER" led_amber.png
make_lit "$LED_RED" led_red.png

# Off state: dark desaturated body, faint top edge, no bloom.
magick -size "${S_W}x${S_H}" "gradient:#2a333d-${LED_OFF_BODY}" \
    "$tmp/mask.png" -alpha off -compose CopyOpacity -composite \
    -channel A -evaluate multiply 0.9 +channel \
    -resize 50% -depth 8 PNG32:led_off.png

identify led_off.png led_green.png led_amber.png led_red.png
