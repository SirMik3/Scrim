# Scrim for Linux.
#   make                       build build/scrim and compile the effects
#   make run                   run from the source tree (uses ../effects)
#   make install               install to $(PREFIX) (default /usr/local)
#   make install RELEASE=1     leave out effects with a non-commercial (NC) license
# Respects CFLAGS, CPPFLAGS and LDFLAGS from the environment or command line (for distro
# packaging). Shaders are recompiled only when an effect changed; release tarballs ship
# them precompiled, so glslang/spirv-cross are only needed when building from git.
PREFIX  ?= /usr/local
DATADIR ?= $(PREFIX)/share/scrim
RELEASE ?= 0

PKGS       := wayland-client wayland-egl egl glesv2 x11 xrandr xfixes xext json-c libpng libjpeg
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))
CFLAGS     ?= -O2 -g
RP_CFLAGS  := -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Ibuild $(PKG_CFLAGS) \
              -DRP_DATADIR='"$(DATADIR)"'
RP_LIBS    := $(PKG_LIBS) -lm

WAYLAND_SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
XDG_SHELL_XML   := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
LAYER_SHELL_XML := protocols/wlr-layer-shell-unstable-v1.xml

GEN_HEADERS := build/wlr-layer-shell-unstable-v1-client-protocol.h build/xdg-shell-client-protocol.h
OBJS := $(patsubst src/%.c,build/%.o,$(wildcard src/*.c)) \
        build/wlr-layer-shell-unstable-v1-protocol.o build/xdg-shell-protocol.o
EFFECTS ?= ../effects
TOOLS   ?= ../tools

all: build/scrim build/scrim.service effects

build:
	mkdir -p build

build/scrim: $(OBJS)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(RP_LIBS) $(LDLIBS)

build/%.o: src/%.c src/rp.h $(GEN_HEADERS) | build
	$(CC) $(CPPFLAGS) $(RP_CFLAGS) $(CFLAGS) -c -o $@ $<

build/%-protocol.o: build/%-protocol.c
	$(CC) $(CPPFLAGS) $(RP_CFLAGS) $(CFLAGS) -c -o $@ $<

build/scrim.service: scrim.service.in | build
	sed 's|@BINDIR@|$(PREFIX)/bin|g' $< > $@

build/wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_SHELL_XML) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_SHELL_XML) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/xdg-shell-client-protocol.h: $(XDG_SHELL_XML) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/xdg-shell-protocol.c: $(XDG_SHELL_XML) | build
	$(WAYLAND_SCANNER) private-code $< $@

effects:
	@for d in $(EFFECTS)/*/; do \
	    stamp="$$d/build/.stamp"; \
	    if [ -f "$$stamp" ] && [ -z "$$(find "$$d" $(TOOLS)/prelude.glsl $(TOOLS)/effectc -type f \
	            ! -path '*/build/*' -newer "$$stamp" 2>/dev/null)" ]; then continue; fi; \
	    $(TOOLS)/effectc "$$d" || exit 1; \
	done

run: all
	SCRIM_EFFECTS_DIR=$(abspath $(EFFECTS)) build/scrim

install: all
	install -Dm755 build/scrim $(DESTDIR)$(PREFIX)/bin/scrim
	install -Dm644 build/scrim.service $(DESTDIR)$(PREFIX)/lib/systemd/user/scrim.service
	@for d in $(EFFECTS)/*/; do \
	    n=$$(basename "$$d"); \
	    if [ "$(RELEASE)" = 1 ] && grep -Eq '"license"[^,]*NC' "$$d/manifest.json"; then \
	        echo "leaving out $$n (non-commercial license)"; continue; fi; \
	    install -d "$(DESTDIR)$(DATADIR)/effects/$$n/build"; \
	    install -m644 "$$d/manifest.json" "$(DESTDIR)$(DATADIR)/effects/$$n/"; \
	    install -m644 "$$d"/build/*.gles "$(DESTDIR)$(DATADIR)/effects/$$n/build/"; \
	    for f in "$$d"/LICENSE* "$$d"/NOTICE*; do \
	        if [ -e "$$f" ]; then install -m644 "$$f" "$(DESTDIR)$(DATADIR)/effects/$$n/"; fi; done; \
	done

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/scrim $(DESTDIR)$(PREFIX)/lib/systemd/user/scrim.service
	rm -rf $(DESTDIR)$(DATADIR)

clean:
	rm -rf build

.PHONY: all effects run install uninstall clean
