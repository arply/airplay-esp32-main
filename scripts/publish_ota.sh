#!/bin/sh
# Build both firmware variants and put them, with the web pages, into the
# website's online-update folder (docs/web_dsp/FLEET_PLAN.md, F4):
#   <arply-web>/assets/firmware/ota/<version>/{arply_v1.bin,arply_v1_usbdac.bin,www/*.html}
#   <arply-web>/assets/firmware/ota/manifest.json
# Commit and push the website yourself afterwards (only that folder).
#
# Usage: scripts/publish_ota.sh [path/to/arply-web]   (default ../arply-web)
set -eu
cd "$(dirname "$0")/.."
WEB=${1:-../arply-web}
VER=$(tr -d '[:space:]' < version.txt)
OUT="$WEB/assets/firmware/ota/$VER"
[ -d "$WEB/assets/firmware" ] || { echo "no website at $WEB"; exit 1; }
[ -e "$OUT" ] && { echo "$OUT exists already – bump version.txt first"; exit 1; }

mkdir -p "$OUT/www"
for ENV in arply_v1 arply_v1_usbdac; do
  touch CMakeLists.txt # CMake does not notice a changed version.txt
  find managed_components -name .DS_Store -delete 2>/dev/null || true
  pio run -e "$ENV"
  BIN=".pio/build/$ENV/firmware.bin"
  strings "$BIN" | grep -qx "$VER" || { echo "$BIN is not $VER"; exit 1; }
  cp "$BIN" "$OUT/$ENV.bin"
done
cp data/www/*.html "$OUT/www/"

PAGES=$(cd data/www && ls *.html | sed 's/.*/"&"/' | paste -sd, -)
cat > "$WEB/assets/firmware/ota/manifest.json" <<EOF
{
  "version": "$VER",
  "www": [$PAGES],
  "builds": {"arply_v1": "arply_v1.bin", "arply_v1_usbdac": "arply_v1_usbdac.bin"}
}
EOF
echo "Ready in $OUT. Then:"
echo "  git -C $WEB add assets/firmware/ota && git -C $WEB commit -m 'Publish firmware $VER' && git -C $WEB push"
