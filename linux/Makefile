# Scrim for Linux.
#   make                       build build/scrim and compile the effects
#   make run                   run from the source tree (uses ../effects)
#   make install               install to $(PREFIX) (default /usr/local)
#   make install RELEASE=1     leave out effects with a non-commercial (NC) license
PREFIX  ?= /usr/local
DATADIR ?= $(PREFIX)/share/scrim
RELEASE ?= 0

PKGS       := wayland-client wayland-egl egl glesv2 x11 xrandr xfixes json-c libpng libjpeg
PKG_CFLAGS := $(shell pkg-config --cflags $(PKGS))
PKG_LIBS   := $(shell pkg-config --libs $(PKGS))
CFLAGS     ?= -O2 -g
CFLAGS     += -std=c11 -D_GNU_SOURCE -Wall -Wextra -Wno-unused-parameter -Ibuild $(PKG_CFLAGS) \
              -DRP_DATADIR='"$(DATADIR)"'
LDLIBS     += $(PKG_LIBS) -lm

WAYLAND_SCANNER := $(shell pkg-config --variable=wayland_scanner wayland-scanner)
XDG_SHELL_XML   := $(shell pkg-config --variable=pkgdatadir wayland-protocols)/stable/xdg-shell/xdg-shell.xml
LAYER_SHELL_XML := protocols/wlr-layer-shell-unstable-v1.xml

GEN_HEADERS := build/wlr-layer-shell-unstable-v1-client-protocol.h build/xdg-shell-client-protocol.h
OBJS := $(patsubst src/%.c,build/%.o,$(wildcard src/*.c)) \
        build/wlr-layer-shell-unstable-v1-protocol.o build/xdg-shell-protocol.o
EFFECTS ?= ../effects
TOOLS   ?= ../tools

all: build/scrim effects

build:
	mkdir -p build

build/scrim: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

build/%.o: src/%.c src/rp.h $(GEN_HEADERS) | build
	$(CC) $(CFLAGS) -c -o $@ $<

build/%-protocol.o: build/%-protocol.c
	$(CC) $(CFLAGS) -c -o $@ $<

build/wlr-layer-shell-unstable-v1-client-protocol.h: $(LAYER_SHELL_XML) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/wlr-layer-shell-unstable-v1-protocol.c: $(LAYER_SHELL_XML) | build
	$(WAYLAND_SCANNER) private-code $< $@
build/xdg-shell-client-protocol.h: $(XDG_SHELL_XML) | build
	$(WAYLAND_SCANNER) client-header $< $@
build/xdg-shell-protocol.c: $(XDG_SHELL_XML) | build
	$(WAYLAND_SCANNER) private-code $< $@

effects:
	@for d in $(EFFECTS)/*/; do $(TOOLS)/effectc "$$d" || exit 1; done

run: all
	SCRIM_EFFECTS_DIR=$(abspath $(EFFECTS)) build/scrim

install: all
	install -Dm755 build/scrim $(DESTDIR)$(PREFIX)/bin/scrim
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
	rm -f $(DESTDIR)$(PREFIX)/bin/scrim
	rm -rf $(DESTDIR)$(DATADIR)

clean:
	rm -rf build

.PHONY: all effects run install uninstall clean
