#!/usr/bin/env bash
# One-shot: dot-matrix phosphor glyph sprites — note letters A-G, accidental
# signs, octave digits 0-8, and a dash for the no-pitch state. Each glyph is
# rasterized large, box-averaged down to a coarse GW x GH dot grid (a cell
# lights when >= 45% covered), point-upscaled back to blocky 2x, masked with
# a tiled round-dot cell, then phosphor-colorized with a screened bloom and
# downscaled to final size. Run on the dev host; commit the outputs.
set -euo pipefail
cd "$(dirname "$0")"
. ./geometry.sh

FONT=DejaVu-Sans-Bold

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# make_cell CELL_W CELL_H OUT: one round dot (white on black), the tile unit.
make_cell() {
    local cw=$1 ch=$2 out=$3
    local cx=$((cw / 2)) cy=$((ch / 2)) r=$((cw * 3 / 8))
    magick -size "${cw}x${ch}" xc:black -fill white \
        -draw "circle $cx,$cy $cx,$((cy - r))" "$out"
}

# render TEXT GW GH SW SH OUT: TEXT quantized to a GW x GH dot grid inside a
# sprite of SW x SH final pixels. Grid cells must divide the 2x sprite evenly
# so the dot tile registers with the point-upscaled grid.
render() {
    local text=$1 gw=$2 gh=$3 sw=$4 sh=$5 out=$6
    local sw2=$((sw * 2)) sh2=$((sh * 2))
    local cw=$((sw2 / gw)) ch=$((sh2 / gh))

    make_cell "$cw" "$ch" "$tmp/cell.png"
    magick -size "${sw2}x${sh2}" "tile:$tmp/cell.png" "$tmp/tile.png"

    magick -background none -fill white -font "$FONT" -pointsize 400 \
        "label:${text}" -trim +repage \
        -filter box -resize "${gw}x${gh}" \
        -background none -gravity center -extent "${gw}x${gh}" \
        -alpha extract -threshold 45% \
        -filter point -resize "${sw2}x${sh2}!" "$tmp/grid.png"

    magick "$tmp/grid.png" "$tmp/tile.png" \
        -compose multiply -composite "$tmp/dots.png"

    magick -size "${sw2}x${sh2}" "xc:${PHOSPHOR_CORE}" \
        "$tmp/dots.png" -alpha off -compose CopyOpacity -composite \
        "$tmp/core.png"
    magick -size "${sw2}x${sh2}" "xc:${PHOSPHOR_BLOOM}" \
        \( "$tmp/dots.png" -blur 0x6 \) -alpha off -compose CopyOpacity \
        -composite -channel A -evaluate multiply 0.6 +channel "$tmp/bloom.png"
    magick "$tmp/bloom.png" "$tmp/core.png" -compose over -composite \
        -resize 50% -depth 8 "PNG32:$out"
}

for n in A B C D E F G; do
    render "$n" "$GLYPH_GRID_W" "$GLYPH_GRID_H" "$GLYPH_W" "$GLYPH_H" \
        "glyph_${n}.png"
done
render "-" "$GLYPH_GRID_W" "$GLYPH_GRID_H" "$GLYPH_W" "$GLYPH_H" glyph_dash.png

# Accidentals as plain "#"/"b" — the LED-tuner convention, and the music
# glyphs U+266F/266D turn to mush on a 6x8 dot grid.
render "#" "$SIGN_GRID_W" "$SIGN_GRID_H" "$SIGN_W" "$SIGN_H" glyph_sharp.png
render "b" "$SIGN_GRID_W" "$SIGN_GRID_H" "$SIGN_W" "$SIGN_H" glyph_flat.png

for d in 0 1 2 3 4 5 6 7 8; do
    render "$d" "$SIGN_GRID_W" "$SIGN_GRID_H" "$SIGN_W" "$SIGN_H" \
        "digit_${d}.png"
done

identify glyph_*.png digit_*.png
