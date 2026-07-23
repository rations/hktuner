#!/bin/sh
# make-hpkg.sh — build the `hktuner` .hpkg on a running Haiku (x86_64): the
# plugin bundle (DSP .so, native HaikuUI .so, TTLs and the GUI sprite PNGs)
# installed to /boot/system/add-ons/media/LV2/hktuner.lv2.
#
# Needs the sibling LV2-haiku checkout built first — the plugin compiles
# against its staged lv2 headers (LV2_HAIKU, default $HOME/LV2-haiku), and the
# LV2 spec bundles a host needs at runtime ship in the lv2_haiku package.
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
VERSION=0.3.0
REVISION=1
STAGE_PKG="$HERE/stage"

LV2_HAIKU=${LV2_HAIKU:-$HOME/LV2-haiku}
export LV2_HAIKU

pkgman install -y gcc make pkgconfig || true

cd "$ROOT"
rm -rf build "$STAGE_PKG"
make

# The UI is Haiku-only and built conditionally; a bundle without it would load
# but silently fall back to a generic UI, so fail loudly here instead.
test -f build/hktuner.lv2/hktuner_ui.so || {
    echo "error: build/hktuner.lv2/hktuner_ui.so missing — the native GUI did not build" >&2
    exit 1
}

mkdir -p "$STAGE_PKG/add-ons/media/LV2"
cp -r build/hktuner.lv2 "$STAGE_PKG/add-ons/media/LV2/"

cp "$HERE/hktuner.PackageInfo" "$STAGE_PKG/.PackageInfo"
OUT="$HERE/hktuner-$VERSION-$REVISION-x86_64.hpkg"
package create -C "$STAGE_PKG" "$OUT"
echo ">> built $OUT"
