#!/bin/bash
# Runs inside the test container (see test/docker-test.sh).
# Builds Scrim, then runs it under Xvfb (X11) and headless Sway (Wayland),
# saving a screenshot of each to /out.
set -euo pipefail
WALLPAPER=${WALLPAPER:-/out/wallpaper.jpg}
EFFECT=${EFFECT:-rain}
export SCRIM_EFFECTS_DIR=/src/effects
export XDG_CONFIG_HOME=/tmp/config      # don't depend on any user config
mkdir -p /tmp/build && cp -r /src/linux/. /tmp/build/ && cd /tmp/build
make -s clean && make -s build/scrim
cp -r /src/effects /tmp/effects && export SCRIM_EFFECTS_DIR=/tmp/effects
cp -r /src/linux/test/effects/. /tmp/effects/      # test-only effects (never packaged)
for d in /tmp/effects/*/; do /src/tools/effectc "$d" >/dev/null; done
BIN=/tmp/build/build/scrim
ARGS=(--effect "$EFFECT" --wallpaper "$WALLPAPER" --no-pause-on-battery --stats --duration 12 ${EXTRA_ARGS:-})
BACKENDS=${BACKENDS:-"snapshot control x11 wayland"}
export XDG_RUNTIME_DIR=/tmp/xdg && mkdir -p -m 700 $XDG_RUNTIME_DIR

# expect PATTERN COMMAND...: runs COMMAND, fails the test unless its output matches.
expect() {
    local want=$1 out cmd; shift
    cmd="$*" && cmd=${cmd//$BIN/scrim}
    out=$("$@" 2>&1) || true
    if grep -q -- "$want" <<<"$out"; then echo "ok: $cmd -> $(grep -m1 -- "$want" <<<"$out")"
    else echo "FAILED: $cmd: expected '$want', got: $out"; exit 1; fi
}

if [[ $BACKENDS == *snapshot* ]]; then
echo "== Snapshots (headless EGL) =="
SNAP=(--wallpaper "$WALLPAPER" --size 640x400 --time 10)
# Persistent passes: the self-test must find no bad state, and its line must sit at
# 10 s * 0.06 = 0.6, which proves uStepDelta adds up exactly over 301 fixed steps.
$BIN --snapshot /out/snapshot-feedback-test.png --effect feedback-test "${SNAP[@]}"
convert /out/snapshot-feedback-test.png -depth 8 rgb:- | python3 /src/linux/test/check-feedback.py 640 400 0.6
# Snapshots must be deterministic.
$BIN --snapshot /out/snapshot-condensation.png --effect condensation "${SNAP[@]}"
$BIN --snapshot /tmp/again.png --effect condensation "${SNAP[@]}" >/dev/null
cmp /out/snapshot-condensation.png /tmp/again.png && echo "condensation: deterministic"
$BIN --snapshot "/out/snapshot-$EFFECT.png" --effect "$EFFECT" "${SNAP[@]}"
fi

if [[ $BACKENDS == *control* ]]; then
echo "== Control (scrim msg) =="
Xvfb :98 -screen 0 640x400x24 >/dev/null 2>&1 &
XV=$!
sleep 1
DISPLAY=:98 $BIN --wallpaper "$WALLPAPER" --no-pause-on-battery --duration 60 2>/tmp/control.log &
APP=$!
for _ in $(seq 50); do [ -S $XDG_RUNTIME_DIR/scrim.sock ] && break; sleep 0.1; done
expect "effect: rain" $BIN msg status
expect "^paused" $BIN msg pause
expect "state: paused" $BIN msg status
expect "^running" $BIN msg toggle
kill -USR1 $APP && sleep 0.3
expect "state: paused" $BIN msg status
expect "^effect plain" $BIN msg effect plain
expect "error: effect 'nope' not found" $BIN msg effect nope
expect "^effect rain" $BIN msg next
expect "error: effect 'rain' has no parameter 'nosuch'" $BIN msg param nosuch=1
expect "^fog = 1" $BIN msg param fog=7                     # clamped to the maximum
expect "^fog = 1" $BIN msg status
expect "already running" env DISPLAY=:98 $BIN --duration 2
expect "unknown command" $BIN msg dance
# Changing a setting in the file wins over an earlier msg change to it.
printf 'effect = condensation\n' > "$XDG_CONFIG_HOME/scrim/scrim.conf" && sleep 0.5
expect "effect: condensation" $BIN msg status
expect "state: paused" $BIN msg status                    # the msg pause still holds
expect "^reloaded" $BIN msg reload
expect "state: running" $BIN msg status
kill $APP && wait $APP || true
expect "doesn't seem to be running" $BIN msg status
test ! -e $XDG_RUNTIME_DIR/scrim.sock && echo "ok: socket removed on exit"
grep -E "paused|resumed|effect:" /tmp/control.log | sed 's/^/  log: /'
rm -f "$XDG_CONFIG_HOME/scrim/scrim.conf"
kill $XV 2>/dev/null || true
fi

if [[ $BACKENDS == *x11* ]]; then
echo "== X11 (Xvfb) =="
Xvfb :99 -screen 0 1280x800x24 >/dev/null 2>&1 &
XVFB=$!
sleep 1
DISPLAY=:99 $BIN "${ARGS[@]}" &
APP=$!
sleep 10
DISPLAY=:99 xwd -root -silent | convert xwd:- "/out/x11-$EFFECT.png"
echo "screenshot: /out/x11-$EFFECT.png"
wait $APP || true
kill $XVFB 2>/dev/null || true
fi

if [[ $BACKENDS == *wayland* ]]; then
echo "== Wayland (headless Sway) =="
printf 'output HEADLESS-1 resolution 1280x800\n' > /tmp/sway.conf
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 \
    sway -c /tmp/sway.conf >/tmp/sway.log 2>&1 &
SWAY=$!
for _ in $(seq 50); do [ -S $XDG_RUNTIME_DIR/wayland-1 ] && break; sleep 0.2; done
export WAYLAND_DISPLAY=wayland-1
$BIN "${ARGS[@]}" &
APP=$!
sleep 10
grim "/out/wayland-$EFFECT.png"
echo "screenshot: /out/wayland-$EFFECT.png"
wait $APP || true
kill $SWAY 2>/dev/null || true
fi
