# Build-and-run test environment: Debian with Mesa's software GL (llvmpipe),
# Xvfb for X11 and a headless Sway for Wayland.
FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
        build-essential pkg-config python3 glslang-tools spirv-cross \
        libwayland-dev wayland-protocols libegl-dev libgles-dev libx11-dev libxrandr-dev \
        libxfixes-dev libjson-c-dev libpng-dev libjpeg-dev \
        libegl-mesa0 libgles2 libgl1-mesa-dri mesa-utils \
        xvfb x11-apps imagemagick sway grim procps \
    && rm -rf /var/lib/apt/lists/*
RUN useradd -m tester
USER tester
WORKDIR /src/linux
