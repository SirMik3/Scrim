#!/bin/bash
# Usage: linux/test/docker-test.sh [wallpaper.jpg] [effect]
# Builds the test image, then builds and runs Scrim for Linux inside it.
# Screenshots land in linux/test/out/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$HERE/out"
mkdir -p "$OUT"
if [ -n "${1:-}" ]; then cp "$1" "$OUT/wallpaper.jpg"; fi
docker build -q -t scrim-linux-test "$HERE" >/dev/null
docker run --rm -v "$ROOT:/src:ro" -v "$OUT:/out" -e EFFECT="${2:-rain}" -e BACKENDS="${BACKENDS:-snapshot wallpaper pausing control x11 wayland}" -e EXTRA_ARGS="${EXTRA_ARGS:-}" \
    scrim-linux-test bash /src/linux/test/run-tests.sh
