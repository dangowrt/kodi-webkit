PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
CFLAGS ?= -O2

DEPS := wpe-webkit-2.0 wayland-client gstreamer-1.0
DEPS_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(DEPS))
DEPS_LIBS := $(shell $(PKG_CONFIG) --libs $(DEPS))

GENERATED := kodi-web-v1-client-protocol.h kodi-web-v1-protocol.c

kodi-webview: kodi-webview.c $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPS_CFLAGS) -I. -o $@ kodi-webview.c kodi-web-v1-protocol.c $(LDFLAGS) $(DEPS_LIBS)

kodi-web-v1-client-protocol.h: protocol/kodi-web-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

kodi-web-v1-protocol.c: protocol/kodi-web-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

install: kodi-webview
	install -D -m 0755 kodi-webview $(DESTDIR)$(BINDIR)/kodi-webview

clean:
	rm -f kodi-webview $(GENERATED)

.PHONY: install clean
