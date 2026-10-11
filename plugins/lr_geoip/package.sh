#!/bin/bash
# Assembles build/package/lr_geoip: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/lr_geoip"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/lr_geoip/liblr_geoip.so" "$OUT/bin/"
cp "$ROOT/build/plugins/lr_geoip/lr_geoip.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
# Files over GitHub's file size limit are kept zipped in dist/.
for f in configs/geoip/GeoLite2-City.mmdb; do
  d="$OUT/$(dirname "$f")"; z="$(basename "$f").zip"
  if command -v unzip >/dev/null; then (cd "$d" && unzip -oq "$z"); else (cd "$d" && python3 -m zipfile -e "$z" .); fi
  rm -f "$d/$z"
done
echo "package: $OUT"
