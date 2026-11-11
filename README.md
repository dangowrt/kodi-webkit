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

The user scripts `kodi-webview` adds to every page are the files in
`scripts/`, installed in `SCRIPTDIR` (`/usr/share/kodi-webkit/scripts`).
Each view reads them when it is created, so an edited script takes effect
with the next web app started.

Content filters arrive in Adblock Plus filter syntax. `kodi-webview-filter`,
installed in `LIBEXECDIR`, converts a list to WebKit content blocker rules
and compiles them into the store under `$XDG_DATA_HOME/kodi-webview-filters`,
keyed by the converted rules, so that a list is compiled once and kept
across runs. It runs as a process of its own because a large list takes
hundreds of megabytes to compile.

Licensed under the GPL-2.0-or-later (LICENSE).
