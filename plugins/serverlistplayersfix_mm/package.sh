#!/bin/bash
# Assembles build/package/serverlistplayersfix_mm: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/serverlistplayersfix_mm"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/serverlistplayersfix_mm/libserverlistplayersfix_mm.so" "$OUT/bin/"
cp "$ROOT/build/plugins/serverlistplayersfix_mm/serverlistplayersfix_mm.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
