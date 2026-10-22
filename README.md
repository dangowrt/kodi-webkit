# kodi-webkit

`kodi-webview`, a WPE WebKit client for Kodi's web app support. Kodi
starts it as a Wayland client and drives its pages over the
`kodi_web_v1` protocol (`protocol/`, MIT, from Kodi).

Build with `make` and `make install`; needs WPE WebKit 2.x, wayland-client
and GStreamer.

Licensed under the GPL-2.0-or-later (LICENSE).
