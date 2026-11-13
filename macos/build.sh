#!/bin/zsh
# Builds Scrim.app into macos/build/.
#   ./build.sh              build (all effects, for your own use)
#   ./build.sh --release    build for distribution: leaves out effects whose manifest
#                           license is non-commercial (contains "NC"), e.g. glass-rain
#   ./build.sh --install    also copy the app to ~/Applications (needed for Launch at Login)
#   ./build.sh --run        (re)launch the built (or installed) app afterwards
#   ./build.sh --universal  build for both Apple silicon and Intel (default: this Mac only)
#   ./build.sh --package    also write build/Scrim-<version>-macos.zip and .dmg
# Flags can be combined.
# Environment: SDKROOT forces a macOS SDK. SCRIM_VERSION overrides the VERSION file,
# BUILD_NUMBER sets CFBundleVersion. SIGN_IDENTITY (e.g. "Developer ID Application: …")
# signs with the hardened runtime for notarization; otherwise the app is ad-hoc signed.
set -euo pipefail

RELEASE=0 INSTALL=0 RUN=0 UNIVERSAL=0 PACKAGE=0
for arg in "$@"; do
    case $arg in
        --release) RELEASE=1 ;;
        --install) INSTALL=1 ;;
        --run) RUN=1 ;;
        --universal) UNIVERSAL=1 ;;
        --package) PACKAGE=1 ;;
        *) echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

HERE=${0:A:h}
ROOT=${HERE:h}
OUT="$HERE/build"
APP="$OUT/Scrim.app"
BIN="$OUT/obj/Scrim"
VERSION=${SCRIM_VERSION:-$(<"$ROOT/VERSION")}
VERSION=${VERSION//\~/-}                      # "0.2.0~dev.5" -> "0.2.0-dev.5" for file names
ARCHS=($(uname -m))
(( UNIVERSAL )) && ARCHS=(arm64 x86_64)

echo "==> Compiling effects"
for dir in "$ROOT"/effects/*/; do
    "$ROOT/tools/effectc" "$dir"
done

echo "==> Compiling Swift"
mkdir -p "$OUT/obj"
compile() {   # sdk arch
    swiftc -O -swift-version 6 -sdk "$1" -target "$2-apple-macos14.0" -module-name Scrim \
        "$HERE"/Sources/Scrim/*.swift -o "$BIN-$2"
}
if [[ -n "${SDKROOT:-}" ]]; then
    SDK=$SDKROOT
    compile "$SDK" "${ARCHS[1]}"
else
    # Prefer the default SDK; if it doesn't match the installed compiler (a common
    # half-updated Command Line Tools state), fall back to older SDKs.
    sdks=("$(xcrun --sdk macosx --show-sdk-path)" ${(On)$(print -l /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.[0-9]*.sdk(N))})
    SDK=""
    for sdk in $sdks; do
        if compile "$sdk" "${ARCHS[1]}" 2>"$OUT/obj/swiftc.log"; then
            echo "    using SDK ${sdk:t}"; SDK=$sdk; break
        fi
        grep -q "not supported by the compiler" "$OUT/obj/swiftc.log" || { cat "$OUT/obj/swiftc.log"; exit 1; }
    done
    [[ -n $SDK ]] || { cat "$OUT/obj/swiftc.log"; exit 1; }
fi
for arch in "${ARCHS[@]:1}"; do compile "$SDK" "$arch"; done
lipo -create "${ARCHS[@]/#/$BIN-}" -output "$BIN"
echo "    architectures: $(lipo -archs "$BIN")"

echo "==> Bundling $APP"
rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources/effects"
cp "$BIN" "$APP/Contents/MacOS/Scrim"
cp "$HERE/Resources/Info.plist" "$APP/Contents/Info.plist"
plutil -replace CFBundleShortVersionString -string "${VERSION%%-*}" "$APP/Contents/Info.plist"
plutil -replace CFBundleVersion -string "${BUILD_NUMBER:-1}" "$APP/Contents/Info.plist"
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
if [[ -n "${SIGN_IDENTITY:-}" ]]; then
    codesign --force --options runtime --timestamp --sign "$SIGN_IDENTITY" "$APP"
    echo "    signed: $SIGN_IDENTITY"
else
    codesign --force --sign - "$APP" >/dev/null   # ad-hoc
fi
echo "    done: Scrim $VERSION ($(du -sh "$APP" | cut -f1))"

if (( PACKAGE )); then
    echo "==> Packaging"
    ZIP="$OUT/Scrim-$VERSION-macos.zip"
    DMG="$OUT/Scrim-$VERSION-macos.dmg"
    rm -f "$ZIP" "$DMG"
    ditto -c -k --sequesterRsrc --keepParent "$APP" "$ZIP"
    STAGE="$OUT/dmg-stage"
    rm -rf "$STAGE" && mkdir -p "$STAGE"
    cp -R "$APP" "$STAGE/"
    ln -s /Applications "$STAGE/Applications"
    hdiutil create -quiet -volname "Scrim $VERSION" -srcfolder "$STAGE" -ov -format UDZO "$DMG"
    rm -rf "$STAGE"
    [[ -n "${SIGN_IDENTITY:-}" ]] && codesign --force --timestamp --sign "$SIGN_IDENTITY" "$DMG"
    echo "    $ZIP"
    echo "    $DMG"
fi

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
