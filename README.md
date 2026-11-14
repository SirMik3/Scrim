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

## Download

Packages for every release are on the [Releases page](https://github.com/SirMik3/Scrim/releases):

| Platform | Package | Install |
|---|---|---|
| macOS 14+ (Apple silicon and Intel) | `Scrim-<version>-macos.dmg` / `.zip` | drag to Applications |
| Debian 12, 13 / Ubuntu 24.04, 26.04 | `scrim_<version>-1~<distro>_<arch>.deb` | `sudo apt install ./scrim_….deb` |
| Fedora | `scrim-<version>-1.fc<N>.<arch>.rpm` | `sudo dnf install ./scrim-….rpm` |
| openSUSE Tumbleweed | `scrim-<version>-1.opensuse.<arch>.rpm` | `sudo zypper install ./scrim-….rpm` |
| Arch Linux | `scrim-<version>-1-x86_64.pkg.tar.zst` | `sudo pacman -U ./scrim-….pkg.tar.zst` |

Linux packages exist for x86_64 and aarch64 (Arch: x86_64). They don't start Scrim
automatically. Enable it per user with `systemctl --user enable --now scrim`, or add
`scrim` to your compositor's autostart.

Until the macOS app is signed with a Developer ID, macOS will block the first launch.
Open it once, then go to System Settings → Privacy & Security and choose "Open Anyway".

Effects under non-commercial licences (such as Rain on Glass) aren't in the packages.
They're a separate download, `scrim-effects-noncommercial-<version>.zip`.

## macOS (14+)

```sh
brew install glslang spirv-cross
macos/build.sh --run                # build macos/build/Scrim.app and launch it
macos/build.sh --install --run      # also copy it to ~/Applications (best for Launch at Login)
macos/build.sh --release            # leave out non-commercial effects (see Licenses)
macos/build.sh --universal --package  # Apple silicon + Intel, plus .zip and .dmg
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
wallpaper = ~/Pictures/lake.jpg     # JPEG or PNG; detected from your desktop if unset

[rain]
fog = 0.4
darken = 0.5
```

`scrim --list` shows every effect and its parameters. `scrim --help` shows
the command-line options, which override the file. On Wayland, rendering stops by itself
when the compositor stops asking for frames: the output is covered, off or locked.
It also pauses on battery, which is read from `/sys/class/power_supply`.

Without `wallpaper =`, Scrim draws over the wallpaper your desktop shows. It reads it
from swww, hyprpaper, swaybg (including Sway's `output * bg`), KDE Plasma, XFCE,
GNOME/Cinnamon/Budgie/MATE, `~/.fehbg` or nitrogen, and checks again every 10 seconds,
so changing your wallpaper (or editing the image) carries over. `scrim --print-wallpaper`
shows what it found and where.

### Controlling a running Scrim

`scrim msg` sends a command to the running instance, so you can bind it to a key or a
status-bar button:

```sh
scrim msg toggle            # also: pause, resume (or: kill -USR1 $(pidof scrim))
scrim msg next              # next effect
scrim msg effect condensation
scrim msg param fog=0.8     # a parameter of the current effect
scrim msg status            # effect, state, wallpaper and parameter values
scrim msg reload            # forget msg changes and re-read the config file
```

```ini
# Hyprland
bind = SUPER, F9, exec, scrim msg toggle
# Sway
bindsym $mod+F9 exec scrim msg toggle
```

Changes made with `scrim msg` last until `reload` or a restart; to keep one, put it in
the config file. Editing a setting in the file overrides an earlier `msg` change to
that same setting. `paused = true` in the file pauses too. Only one Scrim runs per
user: a second one exits straight away (with status 0, so a systemd unit doesn't
keep retrying). The socket is `$XDG_RUNTIME_DIR/scrim.sock`.

**Not done yet on Linux:** a tray icon or settings window, and image formats other
than JPEG/PNG.

### Testing without a Linux machine

`linux/test/docker-test.sh [wallpaper.jpg] [effect]` builds the app in a Debian
container, then runs it under Xvfb (X11) and a headless Sway (Wayland) with Mesa's
software renderer. Screenshots of each go to `linux/test/out/`. It also renders
offscreen snapshots and checks them: `linux/test/effects/feedback-test` (a test-only
effect) verifies persistent passes, and repeated snapshots must be identical.
Set `BACKENDS` to any of `snapshot x11 wayland` to run only some of these.

## Developing effects

```sh
tools/effectc effects/rain                        # after editing a .frag
macos/build/Scrim.app/Contents/MacOS/Scrim \
    --snapshot out.png --effect rain --time 12 --size 1728x1117 \
    --param drops=1 --param rain=0.8              # render one frame offscreen (macOS)
linux/build/scrim --snapshot out.png --effect rain --time 12 --size 1280x800  # Linux
```

`--snapshot` also prints the median time per frame and the memory use. On Linux it
needs no display server (it uses headless EGL), so it works over SSH and in CI.
Effects with persistent passes are simulated from 0 up to `--time` in steps of
1/fps (1/30 s on macOS), so snapshots are reproducible.
User effects go in `~/Library/Application Support/Scrim/Effects/` (macOS) or
`~/.local/share/scrim/effects/` (Linux). You can also point `SCRIM_EFFECTS_DIR`
at a folder. Read the portability rules in the spec. Format 2 adds *persistent passes*, which
read their own output from the previous frame; `effects/condensation` is a small example. In particular, never call
`smoothstep` with its edges reversed: Metal tolerates it, Mesa doesn't.

macOS: `SCRIM_DEBUG=1` logs the frame rate, pause reasons and memory every 5 s.
Linux: `scrim --stats` does the same.

## Building packages and releasing

`.github/workflows/build.yml` runs on every push and pull request. It:

1. builds a source tarball with the shaders precompiled (`packaging/make-source-tarball.sh`);
2. builds a native package inside each distribution's container and **installs and
   smoke-tests it there** (`packaging/ci/build-linux-package.sh`). The recipes are
   `packaging/debian/`, `packaging/rpm/scrim.spec` and `packaging/arch/PKGBUILD`;
3. builds the universal macOS app, `.zip` and `.dmg` (`macos/build.sh --release --universal --package`).

Every run's packages can be downloaded from its "Artifacts" section, with versions like
`0.1.0~dev.42`. To release:

```sh
echo 0.2.0 > VERSION && git commit -am "Release 0.2.0"
git tag v0.2.0 && git push origin main v0.2.0
```

The workflow then creates a **draft** release with all packages and `SHA256SUMS`.
Check it and click Publish. The tag must match `VERSION`.

To test the Linux packaging locally with Docker:

```sh
WORKTREE=1 packaging/make-source-tarball.sh 0.1.0 /tmp/dist
docker run --rm -v "$PWD:/repo:ro" -v /tmp/dist:/dist:ro -v /tmp/out:/out -w /repo fedora:latest \
    bash packaging/ci/build-linux-package.sh /dist/scrim-0.1.0.tar.gz /out
```

**Signed macOS builds (optional):** add the repository secrets `MACOS_CERTIFICATE_P12`
(base64 of a Developer ID Application certificate), `MACOS_CERTIFICATE_PASSWORD`, and for
notarization `APPLE_ID`, `APPLE_TEAM_ID` and `APPLE_APP_PASSWORD`. The workflow then signs
with the hardened runtime, notarizes and staples the `.dmg`. Without them, builds are
ad-hoc signed.

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
