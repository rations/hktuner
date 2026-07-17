# Master geometry for the hktuner screen art. Sourced by every make_*.sh;
# the same numbers are mirrored as constants in the UI source. All values
# are in FINAL (1x) pixels; scripts render at 2x and downscale.
#
# The glass interior was measured off the 1536x1024 master with:
#   magick blank-screen-background.png -colorspace gray -threshold 12% \
#     -negate -trim info:      -> 1202x409+169+298  (bezel-edge box)
# i.e. ~601x204 at +84+149 at the final 0.5x scale; inset a few px for the
# safe interior below.

# Window/background (0.5x of the 1536x1024 master).
WIN_W=768
WIN_H=512

# Shipped view: the canvas around the device is transparent (device opaque
# bbox is 593x197+90+148 in the 768x512 frame), so the UI crops to the
# device plus a black outline margin, with the reference-entry strip on the
# black below it. The UI draws background/sprites shifted by -CROP_X/-CROP_Y.
CROP_X=74
CROP_Y=132
VIEW_W=626
VIEW_H=268

# Glass interior (safe drawing area inside the bezel).
SCREEN_X=90
SCREEN_Y=153
SCREEN_W=588
SCREEN_H=196

# Side LED bar stacks: N_LED_ROWS cells per side, drawn bottom-up.
# Sprites are padded for glow; LED_W/LED_H is the padded sprite size and
# LED_CORE_W/H the lit cell inside it (centered). 7 rows: an 8th row
# (y = 319..339) lands on the bezel corner rounding, outside the glass.
N_LED_ROWS=7
LED_W=48
LED_H=20
LED_CORE_W=40
LED_CORE_H=12
LED_PITCH_Y=22
LED_LEFT_X=106
LED_RIGHT_X=614
LED_STACK_TOP=165

# Cents dot strip: N_DOTS dots ~= -50..+50 cents (2 cents/dot), center dot
# is index N_DOTS/2. Marker sprite is taller and overlays the active dot.
N_DOTS=51
DOT_SPR=12
DOT_PITCH=8
DOT_STRIP_Y=305
DOT_STRIP_X0=184
MARKER_W=16
MARKER_H=32

# Note glyph (dot-matrix letter), accidental sign and octave digit.
GLYPH_W=84
GLYPH_H=108
GLYPH_GRID_W=7
GLYPH_GRID_H=9
GLYPH_X=322
GLYPH_Y=178
SIGN_W=36
SIGN_H=48
SIGN_GRID_W=6
SIGN_GRID_H=8
SIGN_X=416
SIGN_Y=180
DIGIT_X=416
DIGIT_Y=238

# Palette.
PHOSPHOR_CORE='#d8f0ff'
PHOSPHOR_BLOOM='#3aa8ff'
LED_GREEN='#58ff6e'
LED_AMBER='#ffb03a'
LED_RED='#ff4a3a'
LED_OFF_BODY='#1c242c'
