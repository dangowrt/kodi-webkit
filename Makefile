PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
GSTPLUGINDIR ?= $(LIBDIR)/gstreamer-1.0
CFLAGS ?= -O2
# library the web process preloads to load the Widevine CDM, if any
WIDEVINE_PRELOAD ?=

DEPS := wpe-webkit-2.0 wayland-client gstreamer-1.0
DEPS_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(DEPS))
DEPS_LIBS := $(shell $(PKG_CONFIG) --libs $(DEPS))

SINK_DEPS := gstreamer-1.0 gstreamer-audio-1.0
SINK_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(SINK_DEPS))
SINK_LIBS := $(shell $(PKG_CONFIG) --libs $(SINK_DEPS))

GENERATED := kodi-web-v1-client-protocol.h kodi-web-v1-protocol.c

all: kodi-webview libgstkodiaudiosink.so

kodi-webview: kodi-webview.c protocol/kodi-web-audio.h $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPS_CFLAGS) -I. \
		$(if $(WIDEVINE_PRELOAD),-DKW_WIDEVINE_PRELOAD='"$(WIDEVINE_PRELOAD)"') \
		-o $@ kodi-webview.c kodi-web-v1-protocol.c $(LDFLAGS) $(DEPS_LIBS)

libgstkodiaudiosink.so: kodiaudiosink.c protocol/kodi-web-audio.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(SINK_CFLAGS) -I. -o $@ kodiaudiosink.c $(LDFLAGS) $(SINK_LIBS)

kodi-web-v1-client-protocol.h: protocol/kodi-web-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

kodi-web-v1-protocol.c: protocol/kodi-web-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

install: all
	install -D -m 0755 kodi-webview $(DESTDIR)$(BINDIR)/kodi-webview
	install -D -m 0755 libgstkodiaudiosink.so $(DESTDIR)$(GSTPLUGINDIR)/libgstkodiaudiosink.so

clean:
	rm -f kodi-webview libgstkodiaudiosink.so $(GENERATED)

.PHONY: all install clean
