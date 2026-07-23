#!/bin/sh
# One-shot build + install ON A HAIKU MACHINE (run this inside Haiku, not on
# the dev host). Mirrors the sibling ports' build-from-source scripts.
#
#   ./build-from-source.sh
#
# Builds hktuner.lv2 against the sibling LV2-haiku checkout's staged headers
# and installs the bundle into the user's non-packaged add-ons dir, where the
# default LV2_PATH picks it up. Build LV2-haiku first.

set -eu

LV2_HAIKU=${LV2_HAIKU:-$HOME/LV2-haiku}
export LV2_HAIKU

pkgman install -y pkgconfig gcc make

make
make install

echo "done: bundle in $HOME/config/non-packaged/add-ons/media/LV2/hktuner.lv2"
