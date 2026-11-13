# Scrim

A lightweight live-wallpaper app for macOS and Linux. It lays animated shader effects over
your existing wallpaper. The first effects are rain; more are planned. The name comes from
theatre, where a scrim is a gauze backdrop that lighting effects are played onto.

- **Effects** (`effects/`): portable GLSL shader passes plus a `manifest.json`. The
  contract is in [`spec/effect-format.md`](spec/effect-format.md), and both apps run
  the same effect folders.
- **`tools/effectc`**: compiles an effect's GLSL into SPIR-V, Metal and GLSL ES at
  build time, so the apps never ship a shader compiler.
- **macOS app** (`macos/`): Swift + Metal, lives in the menu bar.
- **Linux app** (`linux/`): C + EGL + OpenGL ES 3, configured with a text file.

Both draw in a click-through layer above the system wallpaper and below everything
else, on every display, and stop rendering when the desktop can't be seen.

## macOS (14+)

```sh
brew install glslang spirv-cross
macos/build.sh --run                # build macos/build/Scrim.app and launch it
macos/build.sh --install --run      # also copy it to ~/Applications (best for Launch at Login)
macos/build.sh --release            # leave out non-commercial effects (see Licenses)
```

The menu-bar icon (a cloud) lets you:

- pick an effect and adjust its parameters with auto-generated sliders
- see the effect's author and licence
- pause it
- set the frame rate and Retina resolution
- choose a custom background image (for Aerial or dynamic wallpapers, which can't be read)
- turn on "Pause on Battery" and "Launch at Login"

It pauses when the desktop is fully covered, when the screens sleep or lock, in Low
Power Mode, and on battery (optional, on by default). When paused it leaves a still frame.

`build.sh` uses `swiftc` directly, so Xcode isn't needed. If the default SDK doesn't
match the installed Swift compiler (partly updated Command Line Tools), it falls back
to an older SDK. Set `SDKROOT` to choose one yourself.

## Linux

Wayland compositors with **wlr-layer-shell** are supported: KDE Plasma, Sway, Hyprland,
river, labwc, Wayfire, niri and others. **X11** works with most window managers.
**GNOME on Wayland is not supported** because GNOME doesn't implement layer-shell. There,
Scrim falls back to X11 through XWayland, but it may end up above the desktop.

```sh
# Debian/Ubuntu (tested on Debian 13)
sudo apt install build-essential pkg-config python3 glslang-tools spirv-cross \
    libwayland-dev wayland-protocols libegl-dev libgles-dev libx11-dev libxrandr-dev \
    libxfixes-dev libjson-c-dev libpng-dev libjpeg-dev
# Fedora (untested)
sudo dnf install gcc make pkgconf python3 glslang spirv-tools spirv-cross \
    wayland-devel wayland-protocols-devel mesa-libEGL-devel mesa-libGLES-devel \
    libX11-devel libXrandr-devel libXfixes-devel json-c-devel libpng-devel libjpeg-turbo-devel
# Arch (untested)
sudo pacman -S base-devel python glslang spirv-cross wayland wayland-protocols \
    libglvnd libx11 libxrandr libxfixes json-c libpng libjpeg-turbo

cd linux
make run                         # run from the source tree
sudo make install                # /usr/local/bin/scrim + effects
sudo make install RELEASE=1      # leave out non-commercial effects
```

Start `scrim` from your compositor's autostart, e.g. `exec scrim` in Sway or
Hyprland. With no menu bar, settings live in `~/.config/scrim/scrim.conf`.
A commented template is written there on first run, and **changes apply as soon as
you save the file**:

```ini
effect = rain
fps = 30
pause_on_battery = true
wallpaper = ~/Pictures/lake.jpg     # JPEG or PNG; defaults to GNOME's setting if any

[rain]
fog = 0.4
darken = 0.5
```

`scrim --list` shows every effect and its parameters. `scrim --help` shows
the command-line options, which override the file. On Wayland, rendering stops by itself
when the compositor stops asking for frames: the output is covered, off or locked.
It also pauses on battery, which is read from `/sys/class/power_supply`.

**Not done yet on Linux:** a tray icon or settings window, reading the wallpaper of
desktops other than GNOME/Cinnamon (set `wallpaper =` instead), and formats other
than JPEG/PNG.

### Testing without a Linux machine

`linux/test/docker-test.sh [wallpaper.jpg] [effect]` builds the app in a Debian
container, then runs it under Xvfb (X11) and a headless Sway (Wayland) with Mesa's
software renderer. Screenshots of each go to `linux/test/out/`.

## Developing effects

```sh
tools/effectc effects/rain                        # after editing a .frag
macos/build/Scrim.app/Contents/MacOS/Scrim \
    --snapshot out.png --effect rain --time 12 --size 1728x1117 \
    --param drops=1 --param rain=0.8              # render one frame offscreen (macOS)
```

`--snapshot` also prints the median GPU time per frame and the memory footprint.
User effects go in `~/Library/Application Support/Scrim/Effects/` (macOS) or
`~/.local/share/scrim/effects/` (Linux). You can also point `SCRIM_EFFECTS_DIR`
at a folder. Read the portability rules in the spec. In particular, never call
`smoothstep` with its edges reversed: Metal tolerates it, Mesa doesn't.

macOS: `SCRIM_DEBUG=1` logs the frame rate, pause reasons and memory every 5 s.
Linux: `scrim --stats` does the same.

## Measured cost

**macOS** (M4 Pro, macOS 26, 1728×1117, rain at 30 fps):

| | |
|---|---|
| GPU time | ~0.3–0.4 ms per frame (glass-rain at full 3456×2234: ~1.1 ms) |
| CPU | ~3% of one core while animating, 0% paused |
| Memory | 34 MB paused, ~165 MB while rendering |
| App bundle | ~480 KB |

About 125 MB of that memory is a fixed allocation by Apple's Metal driver. Any process
that renders with the GPU pays it, even a one-triangle test program. Scrim's own
GPU resources come to about 27 MB.

**Linux:** I've only measured inside the test container, using Mesa's *software*
renderer (llvmpipe): 30 fps steady, about 120 MB resident memory, most of it the LLVM
shader compiler. Memory and CPU use with a real GPU driver haven't been measured yet.

## Licenses

Scrim is released under the [MIT License](LICENSE), with these exceptions:

- **`effects/glass-rain/`** is a port of "Heartfelt" by Martijn Steinrucken (BigWIngs),
  licensed **CC BY-NC-SA 3.0**. See its `NOTICE.txt`. It can't be used commercially, and
  changes to it must stay under the same licence. `macos/build.sh --release` and
  `make install RELEASE=1` leave out any effect whose manifest licence contains "NC".
- **`linux/protocols/wlr-layer-shell-unstable-v1.xml`**: © 2017 Drew DeVault, under the
  permissive licence in the file (see `linux/protocols/README.md`).
- The hash functions in `tools/prelude.glsl` are based on "Hash without Sine" by Dave
  Hoskins (MIT); keep that credit.

The `rain` and `plain` effects are original work under MIT.
