#!/bin/zsh
# Compiles and runs the macOS unit tests (macos/tests/) against the app's sources
# that don't need AppKit or Metal. Picks an SDK the same way build.sh does.
set -euo pipefail
HERE=${0:A:h}
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
SOURCES=("$HERE/Sources/Scrim/URLCommand.swift" "$HERE"/tests/*.swift)
sdks=("${SDKROOT:-$(xcrun --sdk macosx --show-sdk-path)}" ${(On)$(print -l /Library/Developer/CommandLineTools/SDKs/MacOSX[0-9]*.[0-9]*.sdk(N))})
for sdk in $sdks; do
    if swiftc -swift-version 6 -sdk "$sdk" -o "$OUT/tests" $SOURCES 2>"$OUT/swiftc.log"; then
        exec "$OUT/tests"
    fi
done
cat "$OUT/swiftc.log" >&2
exit 1
