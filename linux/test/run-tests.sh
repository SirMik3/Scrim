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
BACKENDS=${BACKENDS:-"x11 wayland"}

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
export XDG_RUNTIME_DIR=/tmp/xdg && mkdir -p -m 700 $XDG_RUNTIME_DIR
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
