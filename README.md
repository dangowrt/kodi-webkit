# kodi-webkit

`kodi-webview`, a WPE WebKit client for Kodi's web app support. Kodi
starts it as a Wayland client and drives its pages over the
`kodi_web_v1` protocol (`protocol/`, MIT, from Kodi).

Build with `make` and `make install`; needs WPE WebKit 2.x, wayland-client
and GStreamer.

Where the Widevine CDM only loads with a library preloaded, as on musl with
gcompat's `libgcompat-ifunc.so.0`, name it in `WIDEVINE_PRELOAD` when building:
`kodi-webview` then passes it to the engine for the web process alone, as
`WPE_WIDEVINE_CDM_PRELOAD`, and drops it from the preloads its other children
inherit from Kodi.

Licensed under the GPL-2.0-or-later (LICENSE).
