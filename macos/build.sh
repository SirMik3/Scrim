#!/bin/zsh
# Builds Scrim.app into macos/build/.
#   ./build.sh              build (all effects, for your own use)
#   ./build.sh --release    build for distribution: leaves out effects whose manifest
#                           license is non-commercial (contains "NC"), e.g. glass-rain
#   ./build.sh --install    also copy the app to ~/Applications (needed for Launch at Login)
#   ./build.sh --run        (re)launch the built (or installed) app afterwards
# Flags can be combined. Set SDKROOT to force a particular macOS SDK.
set -euo pipefail

RELEASE=0 INSTALL=0 RUN=0
for arg in "$@"; do
    case $arg in
        --release) RELEASE=1 ;;
        --install) INSTALL=1 ;;
        --run) RUN=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

HERE=${0:A:h}
ROOT=${HERE:h}
OUT="$HERE/build"
APP="$OUT/Scrim.app"
BIN="$OUT/obj/Scrim"

echo "==> Compiling effects"
for dir in "$ROOT"/effects/*/; do
    "$ROOT/tools/effectc" "$dir"
done

echo "==> Compiling Swift"
mkdir -p "$OUT/obj"
compile() {
    swiftc -O -swift-version 6 -sdk "$1" -target arm64-apple-macos14.0 -module-name Scrim \
        "$HERE"/Sources/Scrim/*.swift -o "$BIN"
}
if [[ -n "${SDKROOT:-}" ]]; then
    compile "$SDKROOT"
else
    # Prefer the default SDK; if it doesn't match the installed compiler (a common
    # half-updated Command Line Tools state), fall back to older SDKs.
    sdks=("$(xcrun --sdk macosx --show-sdk-path)" ${(On)$(print -l /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.[0-9]*.sdk(N))})
    built=0
    for sdk in $sdks; do
        if compile "$sdk" 2>"$OUT/obj/swiftc.log"; then
            echo "    using SDK ${sdk:t}"; built=1; break
        fi
        grep -q "not supported by the compiler" "$OUT/obj/swiftc.log" || { cat "$OUT/obj/swiftc.log"; exit 1; }
    done
    (( built )) || { cat "$OUT/obj/swiftc.log"; exit 1; }
fi

echo "==> Bundling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/effects"
cp "$BIN" "$APP/Contents/MacOS/Scrim"
cp "$HERE/Resources/Info.plist" "$APP/Contents/Info.plist"
for dir in "$ROOT"/effects/*/; do
    name=${${dir%/}:t}
    license=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("license",""))' "$dir/manifest.json")
    if (( RELEASE )) && [[ $license == *NC* ]]; then
        echo "    leaving out $name ($license is non-commercial)"
        continue
    fi
    mkdir -p "$APP/Contents/Resources/effects/$name/build"
    cp "$dir/manifest.json" "$APP/Contents/Resources/effects/$name/"
    cp "$dir"/build/*.metal "$APP/Contents/Resources/effects/$name/build/"
    for f in "$dir"/(LICENSE*|NOTICE*)(N); do cp "$f" "$APP/Contents/Resources/effects/$name/"; done   # spec: ship credits
done
codesign --force --sign - "$APP" >/dev/null
echo "    done ($(du -sh "$APP" | cut -f1))"

if (( INSTALL )); then
    DEST="$HOME/Applications/Scrim.app"
    pkill -x Scrim 2>/dev/null || true
    mkdir -p "$HOME/Applications"
    rm -rf "$DEST"
    cp -R "$APP" "$DEST"
    echo "==> Installed $DEST"
    APP="$DEST"
fi

if (( RUN )); then
    pkill -x Scrim 2>/dev/null || true
    open "$APP"
fi
