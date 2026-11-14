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
BACKENDS=${BACKENDS:-"snapshot wallpaper pausing control x11 wayland"}
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

if [[ $BACKENDS == *wallpaper* ]]; then
echo "== Wallpaper detection =="
mkdir -p /tmp/wp /tmp/fake
for n in feh nitrogen kde xfce swww hypr sway light dark "with space"; do cp "$WALLPAPER" "/tmp/wp/$n.jpg"; done
# Fake tools and daemons: detection looks for these process names and runs these commands.
fake() { printf '#!/bin/bash\n%s\n' "$2" > /tmp/fake/$1 && chmod +x /tmp/fake/$1; }
fake swww "echo ': eDP-1: 1920x1080, scale: 1, currently displaying: image: /tmp/wp/swww.jpg'"
fake hyprctl "echo 'eDP-1 = /tmp/wp/hypr.jpg'"
fake gsettings 'case "$3" in color-scheme) echo "'"'"'$SCHEME'"'"'";; picture-uri) echo "'"'"'file:///tmp/wp/light.jpg'"'"'";; picture-uri-dark) echo "'"'"'file:///tmp/wp/dark.jpg'"'"'";; esac'
for d in swww-daemon hyprpaper swaybg; do fake $d 'sleep 60'; done
# detect HOME [VAR=VALUE...] -> scrim --print-wallpaper in a clean environment
detect() { local home=$1; shift; env -i PATH=/tmp/fake:/usr/bin:/bin HOME=$home "$@" $BIN --print-wallpaper; }
home() { rm -rf /tmp/h-$1 && mkdir -p /tmp/h-$1/.config && echo /tmp/h-$1; }

expect "no wallpaper detected" detect "$(home none)"
H=$(home feh); printf "#!/bin/sh\nfeh --no-fehbg --bg-fill '/tmp/wp/with space.jpg' \n" > $H/.fehbg
expect "/tmp/wp/with space.jpg (from feh)" detect $H
H=$(home nitro); mkdir -p $H/.config/nitrogen; printf '[xin_-1]\nfile=/tmp/wp/nitrogen.jpg\nmode=5\n' > $H/.config/nitrogen/bg-saved.cfg
expect "nitrogen.jpg (from nitrogen)" detect $H
H=$(home kde); printf '[Containments][1][General]\nImage=/nope.jpg\n\n[Containments][1][Wallpaper][org.kde.image][General]\nImage=file:///tmp/wp/with%%20space.jpg\n' > $H/.config/plasma-org.kde.plasma.desktop-appletsrc
expect "with space.jpg (from KDE Plasma)" detect $H XDG_CURRENT_DESKTOP=KDE
expect "no wallpaper detected" detect $H XDG_CURRENT_DESKTOP=sway       # KDE's file only counts on KDE
H=$(home xfce); mkdir -p $H/.config/xfce4/xfconf/xfce-perchannel-xml
printf '<property name="last-image" type="string" value="/tmp/wp/xfce.jpg"/>\n' > $H/.config/xfce4/xfconf/xfce-perchannel-xml/xfce4-desktop.xml
expect "xfce.jpg (from XFCE)" detect $H XDG_CURRENT_DESKTOP=XFCE
expect "light.jpg (from gsettings)" detect "$(home g1)" XDG_CURRENT_DESKTOP=GNOME SCHEME=default
expect "dark.jpg (from gsettings)" detect "$(home g2)" XDG_CURRENT_DESKTOP=GNOME SCHEME=prefer-dark
/tmp/fake/swaybg -o '*' -i /tmp/wp/sway.jpg -m fill & P1=$!
expect "sway.jpg (from swaybg)" detect "$(home s)"
/tmp/fake/hyprpaper & P2=$!
expect "hypr.jpg (from hyprpaper)" detect "$(home s)"
/tmp/fake/swww-daemon & P3=$!
H=$(home both); printf "feh --bg-fill '/tmp/wp/feh.jpg'\n" > $H/.fehbg
expect "swww.jpg (from swww)" detect $H                                # a running daemon beats feh
fake swww "echo 'eDP-1: 1920x1080, scale: 1, currently displaying: color: 000000'"
expect "hypr.jpg (from hyprpaper)" detect $H                           # swww shows a colour: skip it
kill $P1 $P2 $P3 2>/dev/null || true

echo "-- following a wallpaper change while running"
Xvfb :97 -screen 0 320x200x24 >/dev/null 2>&1 &
XV=$!
sleep 1
H=$(home live); printf "feh --bg-fill '/tmp/wp/feh.jpg'\n" > $H/.fehbg
env HOME=$H DISPLAY=:97 XDG_CONFIG_HOME=$H/.config $BIN --no-pause-on-battery --duration 13 2>/tmp/live.log &
APP=$!
sleep 2
printf "feh --bg-fill '/tmp/wp/nitrogen.jpg'\n" > $H/.fehbg
wait $APP || true
kill $XV 2>/dev/null || true
expect "wallpaper: /tmp/wp/feh.jpg (from feh)" cat /tmp/live.log
expect "wallpaper: /tmp/wp/nitrogen.jpg (from feh)" cat /tmp/live.log
fi

if [[ $BACKENDS == *pausing* ]]; then
echo "== Pausing (power, fullscreen, screens off) =="
# A fake /sys: a discharging battery and an unplugged charger.
F=/tmp/fakesys && rm -rf $F && mkdir -p $F/sys/class/power_supply/{BAT0,AC} $F/sys/firmware/acpi
echo Battery > $F/sys/class/power_supply/BAT0/type && echo Discharging > $F/sys/class/power_supply/BAT0/status
echo Mains > $F/sys/class/power_supply/AC/type && echo 0 > $F/sys/class/power_supply/AC/online
echo balanced > $F/sys/firmware/acpi/platform_profile
Xvfb :96 -screen 0 640x400x24 +extension DPMS >/dev/null 2>&1 &
XV=$!
sleep 1
export DISPLAY=:96
SCRIM_SYS_ROOT=$F $BIN --wallpaper "$WALLPAPER" --stats --duration 60 2>/tmp/pausing.log &
APP=$!
for _ in $(seq 50); do [ -S $XDG_RUNTIME_DIR/scrim.sock ] && break; sleep 0.1; done
sleep 0.5
expect "state: on battery: paused" $BIN msg status
expect "running (but on battery: paused)" $BIN msg resume
echo 1 > $F/sys/class/power_supply/AC/online && sleep 5.5
expect "state: running" $BIN msg status
echo low-power > $F/sys/firmware/acpi/platform_profile && sleep 5.5
expect "state: power saver: paused" $BIN msg status
echo balanced > $F/sys/firmware/acpi/platform_profile
mkdir -p $F/var/lib/power-profiles-daemon && printf '[State]\nProfile=power-saver\n' > $F/var/lib/power-profiles-daemon/state.ini && sleep 5.5
expect "state: power saver: paused" $BIN msg status
rm $F/var/lib/power-profiles-daemon/state.ini && sleep 5.5
expect "state: running" $BIN msg status

# The active window goes fullscreen over the whole screen (EWMH, as a WM would set it).
xclock -geometry 640x400+0+0 & CLOCK=$!
sleep 1
W=$(xwininfo -name xclock -int | sed -n 's/.*Window id: \([0-9]*\).*/\1/p')
xprop -root -f _NET_ACTIVE_WINDOW 32c -set _NET_ACTIVE_WINDOW "$W"
xprop -id "$W" -f _NET_WM_STATE 32a -set _NET_WM_STATE _NET_WM_STATE_FULLSCREEN && sleep 0.5
expect "covered by a fullscreen window: pausing it" cat /tmp/pausing.log
xprop -id "$W" -remove _NET_WM_STATE && sleep 0.5
expect "no longer covered by a fullscreen window" cat /tmp/pausing.log
kill $CLOCK && sleep 0.5                         # the active window vanishes: no crash
expect "state: running" $BIN msg status

if xset q | grep -q "DPMS is"; then
    xset dpms force off && sleep 5.5
    expect "state: screens off: paused" $BIN msg status
    xset dpms force on && sleep 5.5
    expect "state: running" $BIN msg status
else
    echo "skipped: this X server has no DPMS (Xvfb), so 'screens off' isn't tested here"
fi
kill $APP && wait $APP || true
grep -E "paused|resumed|x11:" /tmp/pausing.log | sed 's/^/  log: /'
kill $XV 2>/dev/null || true
unset DISPLAY
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
export SWAYSOCK=$(ls $XDG_RUNTIME_DIR/sway-ipc.*.sock | head -1)
$BIN "${ARGS[@]}" 2>/tmp/wayland.log &
APP=$!
sleep 8
# Hotplug a larger monitor: Scrim must cover it and re-decode the wallpaper at its size.
swaymsg -q create_output && sleep 0.5
NEW=$(swaymsg -t get_outputs | grep -o '"name": "HEADLESS-[0-9]*"' | cut -d'"' -f4 | grep -v '^HEADLESS-1$' | head -1)
swaymsg -q output "$NEW" resolution 2560x1600 && sleep 2
grim -o HEADLESS-1 "/out/wayland-$EFFECT.png"
grim -o "$NEW" "/out/wayland-$EFFECT-hotplug.png"
echo "screenshots: /out/wayland-$EFFECT.png, /out/wayland-$EFFECT-hotplug.png"
wait $APP || true
cat /tmp/wayland.log >&2
expect "(2560x" grep wallpaper: /tmp/wayland.log
kill $SWAY 2>/dev/null || true
fi
