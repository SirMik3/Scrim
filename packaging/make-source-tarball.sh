#!/usr/bin/env bash
# Usage: packaging/make-source-tarball.sh VERSION OUTDIR
# Writes OUTDIR/scrim-VERSION.tar.gz from the committed tree (git HEAD), with every
# effect's shaders precompiled, so packages build without glslang/spirv-cross.
# Also writes OUTDIR/scrim-effects-noncommercial-VERSION.zip: effects whose license
# is non-commercial (left out of packages), compiled, with their NOTICE files.
set -euo pipefail
VERSION=$1
OUT=$(mkdir -p "$2" && cd "$2" && pwd)
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

NAME=scrim-$VERSION
export COPYFILE_DISABLE=1   # macOS tar: no AppleDouble ._ files
TAR_CREATE=(tar)
tar --version 2>/dev/null | grep -q bsdtar && TAR_CREATE=(tar --no-mac-metadata)   # no Apple xattrs
if [ "${WORKTREE:-0}" = 1 ]; then
    # Local testing: use the working tree (tracked + untracked, minus .gitignore'd files).
    mkdir -p "$WORK/$NAME"
    (cd "$ROOT" && git ls-files -z -co --exclude-standard | xargs -0 tar -cf -) | tar -x -C "$WORK/$NAME"
else
    git -C "$ROOT" archive --format=tar --prefix="$NAME/" HEAD | tar -x -C "$WORK"
fi
echo "$VERSION" > "$WORK/$NAME/VERSION"
for d in "$WORK/$NAME"/effects/*/; do "$WORK/$NAME/tools/effectc" "$d" >/dev/null; done
"${TAR_CREATE[@]}" -C "$WORK" -czf "$OUT/$NAME.tar.gz" "$NAME"
echo "wrote $OUT/$NAME.tar.gz"

nc=()
for d in "$WORK/$NAME"/effects/*/; do
    if python3 -c 'import json,sys; sys.exit(0 if "NC" in json.load(open(sys.argv[1])).get("license","") else 1)' "$d/manifest.json"; then
        nc+=("$(basename "$d")")
    fi
done
if ((${#nc[@]})); then
    mkdir -p "$WORK/nc"
    for n in "${nc[@]}"; do
        mkdir -p "$WORK/nc/$n/build"
        cp "$WORK/$NAME/effects/$n"/manifest.json "$WORK/$NAME/effects/$n"/*.frag "$WORK/nc/$n/"
        cp "$WORK/$NAME/effects/$n"/build/*.{gles,metal,spv} "$WORK/nc/$n/build/"
        cp "$WORK/$NAME/effects/$n"/{LICENSE,NOTICE}* "$WORK/nc/$n/" 2>/dev/null || true
    done
    cat > "$WORK/nc/README.txt" <<TXT
Scrim effects under non-commercial licenses (not included in the Scrim packages).
Each folder has its own NOTICE with the author and license terms.

Install by copying the effect folder into:
  macOS: ~/Library/Application Support/Scrim/Effects/
  Linux: ~/.local/share/scrim/effects/
then pick it from the menu (macOS) or set "effect = <name>" in ~/.config/scrim/scrim.conf.
TXT
    (cd "$WORK/nc" && zip -qr "$OUT/scrim-effects-noncommercial-$VERSION.zip" .)
    echo "wrote $OUT/scrim-effects-noncommercial-$VERSION.zip (${nc[*]})"
fi
