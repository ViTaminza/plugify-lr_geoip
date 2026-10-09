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
echo "package: $OUT"
