PKG_CONFIG ?= pkg-config
WAYLAND_SCANNER ?= wayland-scanner
PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib
LIBEXECDIR ?= $(PREFIX)/libexec
GSTPLUGINDIR ?= $(LIBDIR)/gstreamer-1.0
SCRIPTDIR ?= $(PREFIX)/share/kodi-webkit/scripts
CFLAGS ?= -O2
# library the web process preloads to load the Widevine CDM, if any
WIDEVINE_PRELOAD ?=

DEPS := wpe-webkit-2.0 wayland-client gstreamer-1.0
DEPS_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(DEPS))
DEPS_LIBS := $(shell $(PKG_CONFIG) --libs $(DEPS))

FILTER_DEPS := wpe-webkit-2.0
FILTER_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(FILTER_DEPS))
FILTER_LIBS := $(shell $(PKG_CONFIG) --libs $(FILTER_DEPS))

SINK_DEPS := gstreamer-1.0 gstreamer-audio-1.0
SINK_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(SINK_DEPS))
SINK_LIBS := $(shell $(PKG_CONFIG) --libs $(SINK_DEPS))

VSINK_DEPS := gstreamer-1.0 gstreamer-video-1.0 gstreamer-allocators-1.0
VSINK_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(VSINK_DEPS))
VSINK_LIBS := $(shell $(PKG_CONFIG) --libs $(VSINK_DEPS))

GENERATED := kodi-web-v1-client-protocol.h kodi-web-v1-protocol.c

all: kodi-webview kodi-webview-filter libgstkodiaudiosink.so libgstkodivideosink.so

kodi-webview: kodi-webview.c protocol/kodi-web-audio.h protocol/kodi-web-video.h $(GENERATED)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(DEPS_CFLAGS) -I. \
		-DKW_FILTER_HELPER='"$(LIBEXECDIR)/kodi-webview-filter"' \
		-DKW_SCRIPT_DIR='"$(SCRIPTDIR)"' \
		$(if $(WIDEVINE_PRELOAD),-DKW_WIDEVINE_PRELOAD='"$(WIDEVINE_PRELOAD)"') \
		-o $@ kodi-webview.c kodi-web-v1-protocol.c $(LDFLAGS) $(DEPS_LIBS)

kodi-webview-filter: kodi-webview-filter.c filter-convert.c filter-convert.h
	$(CC) $(CPPFLAGS) $(CFLAGS) $(FILTER_CFLAGS) -I. \
		-o $@ kodi-webview-filter.c filter-convert.c $(LDFLAGS) $(FILTER_LIBS)

libgstkodiaudiosink.so: kodiaudiosink.c protocol/kodi-web-audio.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(SINK_CFLAGS) -I. -o $@ kodiaudiosink.c $(LDFLAGS) $(SINK_LIBS)

libgstkodivideosink.so: kodivideosink.c protocol/kodi-web-video.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -fPIC -shared $(VSINK_CFLAGS) -I. -o $@ kodivideosink.c $(LDFLAGS) $(VSINK_LIBS)

kodi-web-v1-client-protocol.h: protocol/kodi-web-v1.xml
	$(WAYLAND_SCANNER) client-header $< $@

kodi-web-v1-protocol.c: protocol/kodi-web-v1.xml
	$(WAYLAND_SCANNER) private-code $< $@

install: all
	install -D -m 0755 kodi-webview $(DESTDIR)$(BINDIR)/kodi-webview
	install -D -m 0755 kodi-webview-filter $(DESTDIR)$(LIBEXECDIR)/kodi-webview-filter
	install -D -m 0755 libgstkodiaudiosink.so $(DESTDIR)$(GSTPLUGINDIR)/libgstkodiaudiosink.so
	install -D -m 0755 libgstkodivideosink.so $(DESTDIR)$(GSTPLUGINDIR)/libgstkodivideosink.so
	install -d $(DESTDIR)$(SCRIPTDIR)
	install -m 0644 scripts/*.js $(DESTDIR)$(SCRIPTDIR)/

clean:
	rm -f kodi-webview kodi-webview-filter libgstkodiaudiosink.so libgstkodivideosink.so $(GENERATED)

.PHONY: all install clean
