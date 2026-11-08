/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Daniel Golle <daniel@makrotopia.org>
 *
 * kodi-webview: engine-side WPEWebKit client for Kodi's WebPlayer.
 *
 * Implements the client side of the kodi_web_v1 Wayland protocol
 * (protocol/kodi-web-v1.xml). Built on the WPE GLib API with the
 * WPEPlatform Wayland backend.
 *
 * Runtime model: Kodi spawns this binary with NO arguments, environment
 * WPE_PLATFORM=wayland and an inherited WAYLAND_SOCKET. The browser is the
 * Wayland client. View state reports (url_changed, title_changed, favicon,
 * load_progress, navigation_state, error, exit, media_state, destroy) are
 * client-to-compositor REQUESTS; the compositor's commands (want_view,
 * content_filter_list, remove_content_filter_list on the manager;
 * set_user_agent, add_user_script, add_content_filter,
 * add_content_filter_list, load_url, navigate, set_active, close on the
 * view) are EVENTS this client handles.
 *
 * Wayland integration: WPE owns the wl_display. We fetch it with
 * wpe_display_wayland_get_wl_display(), create our OWN wl_registry on it to
 * find and bind kodi_web_manager_v1, and do our own roundtrip. We never
 * touch WPE's own registry. The manager/view proxies live on WPE's default
 * event queue and are dispatched by WPE's existing GLib main-loop
 * integration; we flush explicitly after sending requests.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gio/gio.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gst/gst.h>

#include <wayland-client.h>

#include <wpe/webkit.h>
#include <wpe/wpe-platform.h>
#include <wpe/wayland/wpe-wayland.h>

#include "kodi-web-v1-client-protocol.h"
#include "protocol/kodi-web-audio.h"
#include "protocol/kodi-web-video.h"

#ifndef KW_FILTER_HELPER
#define KW_FILTER_HELPER "/usr/libexec/kodi-webview-filter"
#endif

#define KW_FILTER_FILE_PREFIX "ContentRuleList-"
#define KW_FILTER_INDEX_GROUP "filters"
#define KW_PROFILE_MAX 255
/* data-directory, cache-directory, disk-cache-enabled, uplink */
#define KW_SESSION_PROPERTIES_MAX 4

/* ------------------------------------------------------------------ */
/* Globals                                                             */
/* ------------------------------------------------------------------ */

static GApplication *g_app = NULL;
static WPEDisplay *g_display = NULL;
static struct wl_display *g_wl_display = NULL;
static struct wl_registry *g_registry = NULL;
static struct kodi_web_manager_v1 *g_manager = NULL;
static uint32_t g_manager_name = 0;
static GHashTable *g_views = NULL;   /* profile id (char*, verbatim) -> KWView* */
static guint g_view_serial = 0;

static WebKitUserContentFilterStore *g_filter_store = NULL;
static char *g_filter_dir = NULL;
static GKeyFile *g_filter_index = NULL;
static char *g_filter_index_path = NULL;
static GHashTable *g_lists = NULL;
static GHashTable *g_lists_cached = NULL;
static GHashTable *g_list_gen = NULL;
static GQueue g_filter_jobs = G_QUEUE_INIT;
static gboolean g_filter_busy = FALSE;

/* pages show their video on Kodi's plane, which the page covers where it paints */
static gboolean g_video_plane = FALSE;
static gboolean g_disk_cache = TRUE;
static WebKitColor g_transparent = { 0.0, 0.0, 0.0, 0.0 };

/* ------------------------------------------------------------------ */
/* Per-view state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *profile;
    guint serial;
    gboolean ephemeral;             /* profile "default": no on-disk state    */

    WebKitNetworkSession *session;
    WebKitSettings *settings;
    WebKitUserContentManager *ucm;
    WebKitFaviconDatabase *favicon_db;
    WebKitWebView *web_view;

    struct kodi_web_view_v1 *proxy;

    GCancellable *cancellable;      /* cancels pending async on close         */
    GHashTable *filters;
    GHashTable *filter_gen;
    GHashTable *filter_lists;
    GHashTable *lists_waiting;      /* lists not prepared yet, each pending   */
    guint pending;
    char *pending_url;
    gboolean config_failed;
    gboolean loading;
    char *failed_uri;
    gboolean exit_sent;
    GHashTable *media_frames;
    gboolean playing_audio;
    guint media_sent;
    char *last_favicon_hash;        /* sha256 of the last favicon sent        */
    guint favicon_retry_id;         /* repeating poll source after a load     */
    int favicon_tries;              /* icon-fetch attempts for the current load */
    gboolean favicon_done;          /* an icon was delivered for this load    */

    guint viewport_width;           /* CSS viewport the page is written for   */
    guint viewport_height;

    char *current_url;              /* last committed URL, for recovery       */
    gboolean active;
    gboolean crash_recovering;      /* set on crash, cleared on next load     */
    gboolean reload_started;        /* recovery reload has begun loading      */
} KWView;

#define KW_MEDIA_VIDEO   (1u << 0)
#define KW_MEDIA_AUDIO   (1u << 1)
#define KW_MEDIA_PLAYING (1u << 2)
#define KW_MEDIA_SENT    (1u << 3)

/* Every video codec name in WebKit's GStreamer decoder map, as patterns. */
#define KW_HARDWARE_VIDEO_TYPES \
    "*; codecs=\"avc*,x-h264,mp4v*,hvc1*,hev1*,x-h265,vp8*,x-vp8,vp9*,vp09*,x-vp9," \
    "av01*,av1,x-av1,theora\""

/* window.close() of the top document asks Kodi to leave the app */
static const char EXIT_SCRIPT[] =
    "(function () {\n"
    "  var kodi = window.webkit.messageHandlers.kodi;\n"
    "  window.close = function () { kodi.postMessage('exit'); };\n"
    "})();\n";

#define KW_MEDIA_WORLD "kodi-media"

/* runs in its own world: pages can neither see nor post to its handler */
static const char MEDIA_SCRIPT[] =
    "(function () {\n"
    "  var post = window.webkit.messageHandlers.kodimedia;\n"
    "  var token = Math.random().toString(36).slice(2) + Date.now().toString(36);\n"
    "  var media = new Set();\n"
    "  var last = -1;\n"
    "  function bits(m) {\n"
    "    var s = 0;\n"
    "    if (m.networkState === m.NETWORK_EMPTY || m.error)\n"
    "      return 0;\n"
    "    if (m instanceof HTMLVideoElement && m.videoWidth > 0)\n"
    "      s |= 1;\n"
    "    if ((m.audioTracks && m.audioTracks.length) || m.webkitAudioDecodedByteCount > 0)\n"
    "      s |= 2;\n"
    "    if (!m.paused && !m.ended)\n"
    "      s |= 4;\n"
    "    return s;\n"
    "  }\n"
    "  function update(e) {\n"
    "    var s = 0;\n"
    "    if (e.target instanceof HTMLMediaElement)\n"
    "      media.add(e.target);\n"
    "    media.forEach(function (m) { s |= bits(m); });\n"
    "    if (s === last)\n"
    "      return;\n"
    "    last = s;\n"
    "    post.postMessage(token + ':' + s);\n"
    "  }\n"
    "  ['play', 'playing', 'pause', 'ended', 'emptied', 'error', 'loadedmetadata',\n"
    "   'loadeddata', 'timeupdate'].forEach(function (t) {\n"
    "    document.addEventListener(t, update, true);\n"
    "  });\n"
    "  window.addEventListener('pagehide', function () {\n"
    "    post.postMessage(token + ':gone');\n"
    "  }, true);\n"
    "})();\n";

static void kw_flush(void)
{
    if (g_wl_display)
        wl_display_flush(g_wl_display);
}

/* ------------------------------------------------------------------ */
/* GStreamer decoder-rank policy (KODI_WEBVIEW_SOFTWARE_CODECS)         */
/*                                                                     */
/* With 0 (default) every software VIDEO decoder ranks below the       */
/* hardware decoders of its codec, so the engine prefers hardware and  */
/* still offers codecs the box can only decode in software. Ranks are  */
/* process-global, so this is set in this process                      */
/* (gst_plugin_feature_set_rank) and in the                            */
/* environment (GST_PLUGIN_FEATURE_RANK), which GStreamer core applies */
/* at gst_init in each child engine process that inherits our env.     */
/* ------------------------------------------------------------------ */

/* Collect the distinct video media types a factory accepts on its sink. */
static GPtrArray *factory_sink_video_types(GstElementFactory *factory)
{
    GPtrArray *types = g_ptr_array_new_with_free_func(g_free);
    const GList *templates = gst_element_factory_get_static_pad_templates(factory);
    for (const GList *l = templates; l; l = l->next) {
        GstStaticPadTemplate *tmpl = l->data;
        if (tmpl->direction != GST_PAD_SINK || !tmpl->static_caps.string)
            continue;
        g_auto(GStrv) parts = g_strsplit(tmpl->static_caps.string, ";", -1);
        for (guint i = 0; parts && parts[i]; i++) {
            char *p = g_strstrip(parts[i]);
            if (!g_str_has_prefix(p, "video/"))
                continue;
            /* media type is up to the first ',' or whitespace */
            gsize n = strcspn(p, ", \t");
            char *mt = g_strndup(p, n);
            gboolean seen = FALSE;
            for (guint k = 0; k < types->len; k++)
                if (g_strcmp0(g_ptr_array_index(types, k), mt) == 0) { seen = TRUE; break; }
            if (seen)
                g_free(mt);
            else
                g_ptr_array_add(types, mt);
        }
    }
    return types;
}

static gboolean factory_is_video_decoder(GstElementFactory *factory, gboolean *is_hw)
{
    const char *klass = gst_element_factory_get_metadata(factory, GST_ELEMENT_METADATA_KLASS);
    if (!klass)
        return FALSE;
    if (!strstr(klass, "Decoder") || !strstr(klass, "Video"))
        return FALSE;
    if (is_hw)
        *is_hw = strstr(klass, "Hardware") != NULL;
    return TRUE;
}

static void hw_ranks_add(GHashTable *hw_ranks, GstElementFactory *factory)
{
    guint rank = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory));
    const char *type;
    GPtrArray *types;
    gpointer known;
    guint i;

    if (rank == GST_RANK_NONE)
        return;
    types = factory_sink_video_types(factory);
    for (i = 0; i < types->len; i++) {
        type = g_ptr_array_index(types, i);
        known = g_hash_table_lookup(hw_ranks, type);
        if (!known || rank < GPOINTER_TO_UINT(known))
            g_hash_table_insert(hw_ranks, g_strdup(type), GUINT_TO_POINTER(rank));
    }
    g_ptr_array_unref(types);
}

static guint hw_rank_lowest(GHashTable *hw_ranks, GstElementFactory *factory)
{
    GPtrArray *types = factory_sink_video_types(factory);
    guint lowest = G_MAXUINT;
    gpointer known;
    guint i;

    for (i = 0; i < types->len; i++) {
        known = g_hash_table_lookup(hw_ranks, g_ptr_array_index(types, i));
        if (known && GPOINTER_TO_UINT(known) < lowest)
            lowest = GPOINTER_TO_UINT(known);
    }
    g_ptr_array_unref(types);
    return lowest;
}

static void kw_apply_codec_policy(void)
{
    const char *sw = g_getenv("KODI_WEBVIEW_SOFTWARE_CODECS");
    guint hw_rank, rank;

    if (sw && g_strcmp0(sw, "1") == 0) {
        g_message("codec policy: KODI_WEBVIEW_SOFTWARE_CODECS=1, leaving decoder ranks untouched");
        return;
    }

    gst_init(NULL, NULL);
    GstRegistry *registry = gst_registry_get();
    GList *features = gst_registry_get_feature_list(registry, GST_TYPE_ELEMENT_FACTORY);

    /* Pass 1: the lowest hardware decoder rank of each codec. */
    GHashTable *hw_codecs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (GList *l = features; l; l = l->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(l->data);
        gboolean is_hw = FALSE;
        if (!factory_is_video_decoder(factory, &is_hw) || !is_hw)
            continue;
        hw_ranks_add(hw_codecs, factory);
    }

    /* Pass 2: rank software video decoders below the hardware of their codec. */
    GString *env = g_string_new(NULL);
    GPtrArray *demoted = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = features; l; l = l->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(l->data);
        gboolean is_hw = FALSE;
        if (!factory_is_video_decoder(factory, &is_hw) || is_hw)
            continue;
        hw_rank = hw_rank_lowest(hw_codecs, factory);
        rank = gst_plugin_feature_get_rank(GST_PLUGIN_FEATURE(factory));
        if (hw_rank == G_MAXUINT || rank < hw_rank)
            continue;

        gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), hw_rank - 1);
        const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        g_ptr_array_add(demoted, g_strdup_printf("%s %u -> %u", name, rank, hw_rank - 1));
        if (env->len)
            g_string_append_c(env, ',');
        g_string_append_printf(env, "%s:%u", name, hw_rank - 1);
    }

    if (demoted->len) {
        /* One factory per line, plus a count, so syslog does not truncate the
         * decision (the GST_PLUGIN_FEATURE_RANK string itself can exceed 1 KB). */
        g_message("codec policy: demoting %u software video decoder(s)", demoted->len);
        for (guint i = 0; i < demoted->len; i++)
            g_message("codec policy: demoted %s", (const char *)g_ptr_array_index(demoted, i));

        /* Propagate to child engine processes via the environment. */
        const char *existing = g_getenv("GST_PLUGIN_FEATURE_RANK");
        char *merged = (existing && *existing)
            ? g_strdup_printf("%s,%s", existing, env->str)
            : g_strdup(env->str);
        g_setenv("GST_PLUGIN_FEATURE_RANK", merged, TRUE);
        g_free(merged);
    } else {
        g_message("codec policy: hardware video decoders already rank first");
    }

    g_ptr_array_free(demoted, TRUE);
    g_string_free(env, TRUE);
    g_hash_table_destroy(hw_codecs);
    gst_plugin_feature_list_free(features);
}

/* ------------------------------------------------------------------ */
/* User agent validation (RFC 7231 field-value, strict ASCII)          */
/* ------------------------------------------------------------------ */

static gboolean ua_is_valid(const char *s)
{
    if (!s || !*s)
        return FALSE;
    int depth = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        unsigned char c = *p;
        if (c == '\t' || c == ' ')
            continue;
        if (c < 0x20 || c >= 0x7f)   /* bare control or non-ASCII/obs-text */
            return FALSE;
        if (c == '(')
            depth++;
        else if (c == ')') {
            if (depth == 0)
                return FALSE;        /* unbalanced comment */
            depth--;
        }
    }
    return depth == 0;
}

/* ------------------------------------------------------------------ */
/* Favicon wl_shm buffer                                               */
/* ------------------------------------------------------------------ */

typedef struct {
    void *data;
    size_t size;
} KWFaviconBuffer;

/* Anonymous, unlinked backing file for a wl_shm pool. */
static int kw_create_shm_file(size_t size)
{
    const char *dir = g_getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        dir = "/tmp";
    char *tmpl = g_build_filename(dir, "kodi-webview-shm-XXXXXX", NULL);
    int fd = g_mkstemp(tmpl);
    if (fd >= 0) {
        unlink(tmpl);
        if (ftruncate(fd, (off_t)size) != 0) {
            close(fd);
            fd = -1;
        }
    }
    g_free(tmpl);
    return fd;
}

static void favicon_buffer_release(void *data, struct wl_buffer *buffer)
{
    KWFaviconBuffer *fb = data;
    wl_buffer_destroy(buffer);
    if (fb) {
        if (fb->data && fb->data != MAP_FAILED)
            munmap(fb->data, fb->size);
        g_free(fb);
    }
}

static const struct wl_buffer_listener favicon_buffer_listener = {
    favicon_buffer_release
};

/*
 * WebKitImage pixels are premultiplied 32-bit ARGB (Skia kBGRA_8888, i.e.
 * cairo ARGB32 layout): byte order B,G,R,A on little-endian, which is exactly
 * what WL_SHM_FORMAT_ARGB8888 means, so the bytes copy across unchanged.
 *
 * Ownership: `bytes` is borrowed. webkit_image_as_bytes() is (transfer none)
 * (it returns the WebKitImage's own GBytes), and webkit_image_list_get() is
 * (transfer none) (the list owns the image). We therefore never unref either;
 * the only owned reference is the WebKitImageList, unreffed by the caller.
 */
static void send_favicon_buffer(KWView *v, WebKitImage *image, GBytes *bytes)
{
    struct wl_shm *shm = wpe_display_wayland_get_wl_shm(WPE_DISPLAY_WAYLAND(g_display));
    if (!shm) {
        g_message("favicon: %s send aborted: no wl_shm on the display", v->profile);
        return;
    }

    int width = webkit_image_get_width(image);
    int height = webkit_image_get_height(image);
    guint stride = webkit_image_get_stride(image);
    if (width <= 0 || height <= 0 || stride < (guint)width * 4) {
        g_message("favicon: %s send aborted: bad geometry %dx%d stride %u",
            v->profile, width, height, stride);
        return;
    }

    /*
     * webkit_image_as_bytes() returns Skia's SkPixmap::computeByteSize(), which
     * is (height-1)*stride + width*4, i.e. the last row is not padded. The wl_shm
     * pool, however, must be stride*height so the compositor can address every
     * row at row*stride. Size the pool to stride*height, require the source to
     * hold at least the computed minimum, and copy only the bytes we have; the
     * trailing pad of the last row stays zero.
     */
    gsize src_len = 0;
    const guint8 *src = g_bytes_get_data(bytes, &src_len);
    size_t pool_size = (size_t)stride * (size_t)height;
    size_t min_src = (size_t)(height - 1) * stride + (size_t)width * 4;
    if (!src || src_len < min_src) {
        g_message("favicon: %s send aborted: short pixel buffer %zu < %zu",
            v->profile, (size_t)src_len, min_src);
        return;
    }

    int fd = kw_create_shm_file(pool_size);
    if (fd < 0) {
        g_message("favicon: %s send aborted: shm file failed: %s",
            v->profile, g_strerror(errno));
        return;
    }

    void *map = mmap(NULL, pool_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (map == MAP_FAILED) {
        g_message("favicon: %s send aborted: mmap failed: %s", v->profile, g_strerror(errno));
        close(fd);
        return;
    }
    memcpy(map, src, src_len);

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)pool_size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
        (int32_t)stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    if (!buffer) {
        g_message("favicon: %s send aborted: wl_buffer creation failed", v->profile);
        munmap(map, pool_size);
        return;
    }

    KWFaviconBuffer *fb = g_new0(KWFaviconBuffer, 1);
    fb->data = map;
    fb->size = pool_size;
    wl_buffer_add_listener(buffer, &favicon_buffer_listener, fb);

    kodi_web_view_v1_favicon(v->proxy, buffer);
    kw_flush();
    g_message("favicon: %s issued favicon(wl_buffer) %dx%d stride %u, flushed",
        v->profile, width, height, stride);
}

/*
 * This WPE 2.54 build exposes NO favicon-change signal (WebKitFaviconDatabase
 * has no "favicon-changed" signal and WebKitWebView has no "favicon" property;
 * both appear only in stale doc comments). The real API is the async
 * get_page_icons -> WebKitImageList, backed by the favicon database that
 * webkit_website_data_manager_set_favicons_enabled() opens. Favicons are off
 * by default and are enabled per view in view_create.
 *
 * get_page_icons returns an empty list immediately (it does not wait) when
 * the page-URL to icon-URL mapping has not yet been populated, which happens
 * only once the engine has seen and started loading the page's icon. We
 * therefore poll from LOAD_FINISHED: an immediate fetch plus up to
 * KW_FAVICON_MAX_TRIES one-second retries, stopping as soon as an icon is
 * delivered. favicon() is emitted only when the icon bytes change (sha256).
 *
 * Ownership (verified against the WPE 2.54 source): get_page_icons_finish
 * returns the WebKitImageList as (transfer full), so we unref it once.
 * webkit_image_list_get and webkit_image_as_bytes are both (transfer none);
 * their results are owned by the list/image, so we never unref them.
 */
#define KW_FAVICON_MAX_TRIES 6

static void favicon_stop_poll(KWView *v)
{
    if (v->favicon_retry_id) {
        g_source_remove(v->favicon_retry_id);
        v->favicon_retry_id = 0;
    }
}

static void on_page_icons_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
    KWView *v = user_data;
    const char *uri = v->web_view ? webkit_web_view_get_uri(v->web_view) : "";
    GError *error = NULL;
    WebKitImageList *list = webkit_favicon_database_get_page_icons_finish(
        WEBKIT_FAVICON_DATABASE(source), res, &error);
    if (!list) {
        if (error && g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            /* view torn down; stay quiet */
        } else if (error && g_error_matches(error, WEBKIT_FAVICON_DATABASE_ERROR,
                                            WEBKIT_FAVICON_DATABASE_ERROR_FAVICON_NOT_FOUND)) {
            if (v->favicon_tries >= KW_FAVICON_MAX_TRIES)
                g_message("favicon: %s %s -> none (not found)", v->profile, uri ? uri : "");
        } else {
            g_message("favicon: %s %s -> error: %s", v->profile, uri ? uri : "",
                error ? error->message : "(no message)");
        }
        g_clear_error(&error);
        return;
    }

    gsize n = webkit_image_list_get_length(list);
    if (n > 0) {
        WebKitImage *image = webkit_image_list_get(list, 0);         /* transfer none */
        GBytes *bytes = image ? webkit_image_as_bytes(image) : NULL; /* transfer none */
        if (bytes) {
            gsize len = 0;
            const guchar *data = g_bytes_get_data(bytes, &len);
            char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, len);
            int w = webkit_image_get_width(image);
            int h = webkit_image_get_height(image);
            if (g_strcmp0(hash, v->last_favicon_hash) != 0) {
                g_free(v->last_favicon_hash);
                v->last_favicon_hash = hash;   /* transferred */
                g_message("favicon: %s %s -> %zu icon(s), sending %dx%d",
                    v->profile, uri ? uri : "", (size_t)n, w, h);
                send_favicon_buffer(v, image, bytes);
            } else {
                g_free(hash);
                g_message("favicon: %s %s -> %zu icon(s), unchanged %dx%d",
                    v->profile, uri ? uri : "", (size_t)n, w, h);
            }
        }
        v->favicon_done = TRUE;
        favicon_stop_poll(v);
    } else if (v->favicon_tries >= KW_FAVICON_MAX_TRIES) {
        g_message("favicon: %s %s -> empty list after %d tries",
            v->profile, uri ? uri : "", v->favicon_tries);
    }
    webkit_image_list_unref(list);   /* transfer full: the only owned ref */
}

static void fetch_favicon(KWView *v)
{
    if (!v->web_view)
        return;
    const char *uri = webkit_web_view_get_uri(v->web_view);
    if (!uri || !*uri || g_strcmp0(uri, "about:blank") == 0)
        return;
    if (!v->favicon_db) {
        g_message("favicon: %s %s -> skipped (favicon database not available)",
            v->profile, uri);
        return;
    }
    webkit_favicon_database_get_page_icons(v->favicon_db, uri, v->cancellable,
        on_page_icons_ready, v);
}

static gboolean favicon_poll_cb(gpointer user_data)
{
    KWView *v = user_data;
    if (v->favicon_done || v->favicon_tries >= KW_FAVICON_MAX_TRIES) {
        v->favicon_retry_id = 0;
        return G_SOURCE_REMOVE;
    }
    v->favicon_tries++;
    fetch_favicon(v);
    return G_SOURCE_CONTINUE;
}

/* Begin polling for the current page's favicon (called at LOAD_FINISHED). */
static void favicon_start_poll(KWView *v)
{
    favicon_stop_poll(v);
    v->favicon_done = FALSE;
    v->favicon_tries = 0;
    fetch_favicon(v);   /* immediate attempt */
    v->favicon_retry_id = g_timeout_add_seconds(1, favicon_poll_cb, v);
}

/* ------------------------------------------------------------------ */
/* WebKit signals -> protocol requests                                 */
/* ------------------------------------------------------------------ */

static void url_report(KWView *v, const char *uri)
{
    g_free(v->current_url);
    v->current_url = g_strdup(uri ? uri : "");
    kodi_web_view_v1_url_changed(v->proxy, v->current_url);
    kw_flush();
}

static void on_notify_uri(WebKitWebView *view, GParamSpec *pspec, gpointer user_data)
{
    KWView *v = user_data;

    (void)pspec;
    if (v->loading)
        return;
    url_report(v, webkit_web_view_get_uri(view));
}

static void media_state_send(KWView *v, gboolean force)
{
    gboolean playing = v->playing_audio;
    GHashTableIter it;
    guint bits = 0;
    gpointer value;

    if (v->playing_audio)
        bits |= KW_MEDIA_AUDIO;
    g_hash_table_iter_init(&it, v->media_frames);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        bits |= GPOINTER_TO_UINT(value) & (KW_MEDIA_VIDEO | KW_MEDIA_AUDIO);
        playing |= (GPOINTER_TO_UINT(value) & KW_MEDIA_PLAYING) != 0;
    }
    if (playing)
        bits |= KW_MEDIA_PLAYING;

    if (!force && (v->media_sent & KW_MEDIA_SENT) && (v->media_sent & ~KW_MEDIA_SENT) == bits)
        return;
    v->media_sent = bits | KW_MEDIA_SENT;
    if (!v->proxy)
        return;
    kodi_web_view_v1_media_state(v->proxy, !!(bits & KW_MEDIA_VIDEO), !!(bits & KW_MEDIA_AUDIO),
                                 !(bits & KW_MEDIA_PLAYING));
    kw_flush();
}

static void media_reset(KWView *v)
{
    g_hash_table_remove_all(v->media_frames);
    v->playing_audio = v->web_view && webkit_web_view_is_playing_audio(v->web_view);
    media_state_send(v, TRUE);
}

static void on_notify_playing_audio(WebKitWebView *view, GParamSpec *pspec, gpointer user_data)
{
    KWView *v = user_data;

    (void)pspec;
    v->playing_audio = webkit_web_view_is_playing_audio(view);
    media_state_send(v, FALSE);
}

static void on_media_message(WebKitUserContentManager *ucm, JSCValue *value, gpointer user_data)
{
    KWView *v = user_data;
    char *message;
    guint64 bits;
    char *state;

    (void)ucm;
    if (!jsc_value_is_string(value))
        return;
    message = jsc_value_to_string(value);
    state = strrchr(message, ':');
    if (!state || state == message) {
        g_free(message);
        return;
    }
    *state++ = '\0';

    if (!strcmp(state, "gone"))
        g_hash_table_remove(v->media_frames, message);
    else if (g_ascii_string_to_unsigned(state, 10, 0,
                                        KW_MEDIA_VIDEO | KW_MEDIA_AUDIO | KW_MEDIA_PLAYING,
                                        &bits, NULL))
        g_hash_table_replace(v->media_frames, g_strdup(message), GUINT_TO_POINTER((guint)bits));
    g_free(message);
    media_state_send(v, FALSE);
}

static void view_exit(KWView *v)
{
    if (v->exit_sent || !v->proxy)
        return;
    v->exit_sent = TRUE;
    g_message("view %s: the page asks to leave", v->profile);
    kodi_web_view_v1_exit(v->proxy);
    kw_flush();
}

static void on_exit_message(WebKitUserContentManager *ucm, JSCValue *value, gpointer user_data)
{
    char *message;

    (void)ucm;
    if (!jsc_value_is_string(value))
        return;
    message = jsc_value_to_string(value);
    if (!strcmp(message, "exit"))
        view_exit(user_data);
    g_free(message);
}

static void on_close(WebKitWebView *view, gpointer user_data)
{
    (void)view;
    view_exit(user_data);
}

static void on_notify_title(WebKitWebView *view, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    KWView *v = user_data;
    const char *title = webkit_web_view_get_title(view);
    kodi_web_view_v1_title_changed(v->proxy, title ? title : "");
    kw_flush();
}

static void on_notify_progress(WebKitWebView *view, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    KWView *v = user_data;
    gdouble p = webkit_web_view_get_estimated_load_progress(view);
    kodi_web_view_v1_load_progress(v->proxy, (uint32_t)(p * 100.0));
    kw_flush();
}

/* The engine's error page for a failed load commits under the failing URI. */
static void load_committed(KWView *v, const char *uri)
{
    gboolean error_page = v->failed_uri && !g_strcmp0(uri, v->failed_uri);

    g_clear_pointer(&v->failed_uri, g_free);
    media_reset(v);
    if (!error_page)
        url_report(v, uri);
}

static void on_load_changed(WebKitWebView *view, WebKitLoadEvent event, gpointer user_data)
{
    KWView *v = user_data;

    /* A recovery reload has actually begun: a further crash now is a real
     * re-crash, not a duplicate of the original termination signal. */
    if (event == WEBKIT_LOAD_STARTED) {
        if (v->crash_recovering)
            v->reload_started = TRUE;
        /* New navigation: stop polling the previous page's favicon. */
        favicon_stop_poll(v);
        v->favicon_done = FALSE;
        v->loading = TRUE;
    }

    if (event == WEBKIT_LOAD_COMMITTED)
        load_committed(v, webkit_web_view_get_uri(view));

    if (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED) {
        kodi_web_view_v1_navigation_state(v->proxy,
            webkit_web_view_can_go_back(view) ? 1 : 0,
            webkit_web_view_can_go_forward(view) ? 1 : 0);
    }

    if (event == WEBKIT_LOAD_FINISHED) {
        v->loading = FALSE;
        v->crash_recovering = FALSE;     /* a load completed: recovery ok  */
        v->reload_started = FALSE;

        /* No favicon-change signal in this build: poll the favicon database
         * from here (the icon may register slightly after load). */
        favicon_start_poll(v);
    }
    kw_flush();
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event,
    const char *failing_uri, GError *error, gpointer user_data)
{
    KWView *v = user_data;

    (void)view;
    /* a provisional load superseded by a newer navigation */
    if (g_error_matches(error, WEBKIT_NETWORK_ERROR, WEBKIT_NETWORK_ERROR_CANCELLED))
        return TRUE;

    if (event == WEBKIT_LOAD_STARTED || event == WEBKIT_LOAD_REDIRECTED) {
        g_free(v->failed_uri);
        v->failed_uri = g_strdup(failing_uri);
    }
    kodi_web_view_v1_error(v->proxy, KODI_WEB_VIEW_V1_ERROR_CODE_LOAD_FAILED,
        (error && error->message) ? error->message : "load failed");
    kw_flush();
    return FALSE;   /* let the engine show its default error page */
}

static gboolean on_load_failed_tls(WebKitWebView *view, const char *failing_uri,
    GTlsCertificate *certificate, GTlsCertificateFlags errors, gpointer user_data)
{
    (void)view;
    (void)failing_uri;
    (void)certificate;
    (void)errors;
    KWView *v = user_data;
    kodi_web_view_v1_error(v->proxy, KODI_WEB_VIEW_V1_ERROR_CODE_TLS_FAILED,
        "TLS handshake or certificate check failed");
    kw_flush();
    return TRUE;    /* handled: do not also emit a generic load_failed */
}

/*
 * requestMediaKeySystemAccess() routes through this permission request before
 * any key-system or configuration check. When unhandled the engine denies the
 * media-key-system request by default, so EME fails with NotSupportedError
 * before the CDM is consulted. Allow the media-key-system request (EME is the
 * point of this client); let every other permission type fall through to the
 * default deny so a TV client is not a blanket-allow.
 */
static gboolean on_permission_request(WebKitWebView *view,
    WebKitPermissionRequest *request, gpointer user_data)
{
    (void)view;
    KWView *v = user_data;
    if (WEBKIT_IS_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(request)) {
        const char *ks = webkit_media_key_system_permission_get_name(
            WEBKIT_MEDIA_KEY_SYSTEM_PERMISSION_REQUEST(request));
        g_message("permission: %s allowing media-key-system %s",
            v->profile, ks ? ks : "(unknown)");
        webkit_permission_request_allow(request);
        return TRUE;
    }
    return FALSE;   /* default deny for all other permission types */
}

static void view_destroy(KWView *v, gboolean send_protocol_destroy);

static void on_web_process_terminated(WebKitWebView *view,
    WebKitWebProcessTerminationReason reason, gpointer user_data)
{
    KWView *v = user_data;
    const char *text = "web process terminated";
    switch (reason) {
    case WEBKIT_WEB_PROCESS_CRASHED:                text = "web process crashed"; break;
    case WEBKIT_WEB_PROCESS_EXCEEDED_MEMORY_LIMIT:  text = "web process exceeded memory limit"; break;
    default:                                        break;
    }

    /*
     * Exactly one error(engine_crashed) per crash episode. This signal can
     * arrive more than once for a single crash; crash_recovering gates the
     * single emission. A second termination is only a genuine re-crash if the
     * recovery load has actually started (reload_started); a duplicate that
     * arrives before the reload begins is ignored so recovery is not aborted.
     */
    if (v->crash_recovering) {
        if (v->reload_started) {
            g_warning("view %s re-crashed during recovery; destroying", v->profile);
            view_destroy(v, TRUE);
        } else {
            g_warning("view %s: duplicate web-process-terminated ignored", v->profile);
        }
        return;
    }

    v->crash_recovering = TRUE;
    v->reload_started = FALSE;
    kodi_web_view_v1_error(v->proxy, KODI_WEB_VIEW_V1_ERROR_CODE_ENGINE_CRASHED, text);
    g_hash_table_remove_all(v->media_frames);
    v->playing_audio = FALSE;
    media_state_send(v, FALSE);
    kw_flush();

    /* Recover in place: reload the current URL. url_changed follows from
     * the load. */
    if (v->current_url && *v->current_url && g_strcmp0(v->current_url, "about:blank") != 0)
        webkit_web_view_load_uri(view, v->current_url);
    else
        webkit_web_view_reload(view);
}

/* ------------------------------------------------------------------ */
/* View event handlers (compositor -> client)                          */
/* ------------------------------------------------------------------ */

static gboolean read_fd_exactly(int fd, uint32_t size, char **out, gsize *out_len)
{
    *out = NULL;
    *out_len = 0;
    if (size == 0)
        return TRUE;
    char *buf = g_malloc(size);
    gsize got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            g_free(buf);
            return FALSE;
        }
        if (n == 0)
            break;          /* short read: fewer bytes than advertised */
        got += (gsize)n;
    }
    *out = buf;
    *out_len = got;
    return TRUE;
}

static gboolean id_valid(const char *id)
{
    const char *p;

    if (!id || !*id || strlen(id) > KW_PROFILE_MAX || !strcmp(id, ".") || !strcmp(id, ".."))
        return FALSE;
    for (p = id; *p; p++)
        if (!g_ascii_isalnum(*p) && *p != '.' && *p != '_' && *p != '-')
            return FALSE;
    return TRUE;
}

static KWView *view_lookup(const char *profile, guint serial)
{
    KWView *v = g_hash_table_lookup(g_views, profile);

    return v && v->serial == serial ? v : NULL;
}

static gboolean filter_id_referenced(const char *id)
{
    gboolean found = FALSE;
    char **keys;
    char *value;
    gsize n = 0;
    gsize i;

    keys = g_key_file_get_keys(g_filter_index, KW_FILTER_INDEX_GROUP, &n, NULL);
    for (i = 0; keys && i < n && !found; i++) {
        value = g_key_file_get_string(g_filter_index, KW_FILTER_INDEX_GROUP, keys[i], NULL);
        found = !g_strcmp0(value, id);
        g_free(value);
    }
    g_strfreev(keys);
    return found;
}

static void filter_file_remove(const char *id)
{
    char *path;

    if (filter_id_referenced(id))
        return;
    path = g_strdup_printf("%s/" KW_FILTER_FILE_PREFIX "%s", g_filter_dir, id);
    g_unlink(path);
    g_free(path);
}

/* Keys are "<profile>/<filter>" and "@list/<list>"; NULL id removes the key. */
static void filter_index_set(const char *key, const char *id)
{
    char *old = g_key_file_get_string(g_filter_index, KW_FILTER_INDEX_GROUP, key, NULL);
    GError *error = NULL;

    if (id)
        g_key_file_set_string(g_filter_index, KW_FILTER_INDEX_GROUP, key, id);
    else
        g_key_file_remove_key(g_filter_index, KW_FILTER_INDEX_GROUP, key, NULL);

    if (!g_key_file_save_to_file(g_filter_index, g_filter_index_path, &error)) {
        g_warning("filters: cannot save %s: %s", g_filter_index_path, error->message);
        g_error_free(error);
    }

    if (old && g_strcmp0(old, id))
        filter_file_remove(old);
    g_free(old);
}

static void filter_store_clean(void)
{
    GDir *dir = g_dir_open(g_filter_dir, 0, NULL);
    const char *name;
    char *path;

    if (!dir)
        return;
    while ((name = g_dir_read_name(dir))) {
        if (!g_str_has_prefix(name, KW_FILTER_FILE_PREFIX) ||
            filter_id_referenced(name + strlen(KW_FILTER_FILE_PREFIX)))
            continue;
        path = g_build_filename(g_filter_dir, name, NULL);
        g_unlink(path);
        g_free(path);
    }
    g_dir_close(dir);
}

static void filter_lists_restore(void)
{
    char **keys;
    gsize n = 0;
    gsize i;

    keys = g_key_file_get_keys(g_filter_index, KW_FILTER_INDEX_GROUP, &n, NULL);
    for (i = 0; keys && i < n; i++) {
        if (!g_str_has_prefix(keys[i], "@list/"))
            continue;
        g_hash_table_replace(g_lists_cached, g_strdup(keys[i] + strlen("@list/")),
            g_key_file_get_string(g_filter_index, KW_FILTER_INDEX_GROUP, keys[i], NULL));
    }
    g_strfreev(keys);
}

static void filter_store_open(void)
{
    g_filter_dir = g_build_filename(g_get_user_data_dir(), "kodi-webview-filters", NULL);
    g_filter_index_path = g_build_filename(g_filter_dir, "index", NULL);
    g_filter_index = g_key_file_new();
    g_lists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_lists_cached = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_list_gen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    if (g_mkdir_with_parents(g_filter_dir, 0700))
        g_warning("filters: cannot create %s: %s", g_filter_dir, g_strerror(errno));
    g_key_file_load_from_file(g_filter_index, g_filter_index_path, G_KEY_FILE_NONE, NULL);
    filter_store_clean();
    filter_lists_restore();
    g_filter_store = webkit_user_content_filter_store_new(g_filter_dir);
}

static void filter_lists_forget_cached(void)
{
    GHashTableIter it;
    gpointer name;
    char *key;

    g_hash_table_iter_init(&it, g_lists_cached);
    while (g_hash_table_iter_next(&it, &name, NULL)) {
        key = g_strconcat("@list/", name, NULL);
        filter_index_set(key, NULL);
        g_free(key);
    }
    g_hash_table_remove_all(g_lists_cached);
}

static void view_pending_done(KWView *v)
{
    char *url;

    if (!v->pending || --v->pending || !v->pending_url)
        return;
    url = v->pending_url;
    v->pending_url = NULL;
    if (!v->config_failed)
        webkit_web_view_load_uri(v->web_view, url);
    g_free(url);
}

static void view_config_fail(KWView *v, const char *what)
{
    g_warning("view %s: %s", v->profile, what);
    g_clear_pointer(&v->pending_url, g_free);
    if (v->config_failed)
        return;
    v->config_failed = TRUE;
    kodi_web_view_v1_error(v->proxy, KODI_WEB_VIEW_V1_ERROR_CODE_CONFIG_FAILED, what);
    kw_flush();
}

static gboolean view_filter_shared(KWView *v, const char *key, const char *id)
{
    GHashTableIter it;
    gpointer other;
    gpointer value;

    g_hash_table_iter_init(&it, v->filters);
    while (g_hash_table_iter_next(&it, &other, &value))
        if (strcmp(other, key) && !strcmp(value, id))
            return TRUE;
    return FALSE;
}

static void view_filter_replace(KWView *v, const char *key, WebKitUserContentFilter *filter)
{
    const char *old = g_hash_table_lookup(v->filters, key);

    if (old && !view_filter_shared(v, key, old))
        webkit_user_content_manager_remove_filter_by_id(v->ucm, old);

    if (!filter) {
        g_hash_table_remove(v->filters, key);
        return;
    }
    webkit_user_content_manager_add_filter(v->ucm, filter);
    g_hash_table_replace(v->filters, g_strdup(key),
                         g_strdup(webkit_user_content_filter_get_identifier(filter)));
    g_message("view %s: content filter %s applied", v->profile, key);
}

typedef struct {
    char *profile;
    guint serial;
    char *key;
    char *id;
    guint generation;
    gboolean counted;
} FilterLoad;

static gboolean filter_load_current(KWView *v, FilterLoad *load)
{
    const char *name = strchr(load->key, ':') + 1;

    if (g_str_has_prefix(load->key, "app:"))
        return GPOINTER_TO_UINT(g_hash_table_lookup(v->filter_gen, name)) == load->generation;
    return !g_strcmp0(g_hash_table_lookup(g_lists, name), load->id);
}

static void on_filter_loaded(GObject *source, GAsyncResult *res, gpointer data)
{
    WebKitUserContentFilter *filter;
    FilterLoad *load = data;
    GError *error = NULL;
    gboolean current;
    char *what;
    KWView *v;

    filter = webkit_user_content_filter_store_load_finish(WEBKIT_USER_CONTENT_FILTER_STORE(source),
                                                          res, &error);
    v = view_lookup(load->profile, load->serial);
    current = v && filter_load_current(v, load);

    if (current && filter) {
        view_filter_replace(v, load->key, filter);
    } else if (current && g_str_has_prefix(load->key, "app:")) {
        what = g_strdup_printf("content filter %s: %s", load->key + 4, error->message);
        view_config_fail(v, what);
        g_free(what);
    } else if (current) {
        g_message("view %s: content filter list %s not applied: %s", v->profile,
                  load->key + 5, error->message);
    }

    if (filter)
        webkit_user_content_filter_unref(filter);
    g_clear_error(&error);
    if (v && load->counted)
        view_pending_done(v);
    g_free(load->profile);
    g_free(load->key);
    g_free(load->id);
    g_free(load);
}

static void view_filter_load(KWView *v, const char *key, const char *id, guint generation,
                             gboolean counted)
{
    FilterLoad *load = g_new0(FilterLoad, 1);

    load->profile = g_strdup(v->profile);
    load->serial = v->serial;
    load->key = g_strdup(key);
    load->id = g_strdup(id);
    load->generation = generation;
    load->counted = counted;
    if (counted)
        v->pending++;
    webkit_user_content_filter_store_load(g_filter_store, id, v->cancellable, on_filter_loaded,
                                          load);
}

typedef struct {
    char *profile;
    guint serial;
    char *name;
    guint generation;
    GBytes *list;
    GSubprocess *proc;
} FilterJob;

static void filter_job_free(FilterJob *job)
{
    g_free(job->profile);
    g_free(job->name);
    g_bytes_unref(job->list);
    g_clear_object(&job->proc);
    g_free(job);
}

/* views waiting for the list apply it now, or fail without one */
static void list_waiters_resolve(const char *name, const char *id)
{
    GHashTableIter it;
    gpointer value;
    char *what;
    char *key;
    KWView *v;

    key = g_strconcat("list:", name, NULL);
    g_hash_table_iter_init(&it, g_views);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        v = value;
        if (!g_hash_table_remove(v->lists_waiting, name))
            continue;
        if (id) {
            view_filter_load(v, key, id, 0, TRUE);
        } else {
            what = g_strdup_printf("content filter list %s cannot be prepared", name);
            view_config_fail(v, what);
            g_free(what);
        }
        view_pending_done(v);
    }
    g_free(key);
}

static void list_report(const char *name, gboolean prepared)
{
    if (wl_proxy_get_version((struct wl_proxy *)g_manager) <
        KODI_WEB_MANAGER_V1_CONTENT_FILTER_LIST_PREPARED_SINCE_VERSION)
        return;
    kodi_web_manager_v1_content_filter_list_prepared(g_manager, name, prepared);
    kw_flush();
}

static void list_prepared(FilterJob *job, const char *id, const char *why)
{
    GHashTableIter it;
    const char *last;
    gpointer value;
    char *key;
    KWView *v;

    if (GPOINTER_TO_UINT(g_hash_table_lookup(g_list_gen, job->name)) != job->generation)
        return;
    if (!id) {
        last = g_hash_table_lookup(g_lists, job->name);
        g_warning("content filter list %s cannot be prepared, %s: %s", job->name,
                  last ? "keeping the last one" : "none to keep", why);
        list_waiters_resolve(job->name, last);
        list_report(job->name, last != NULL);
        return;
    }

    g_hash_table_replace(g_lists, g_strdup(job->name), g_strdup(id));
    key = g_strconcat("@list/", job->name, NULL);
    filter_index_set(key, id);
    g_free(key);

    key = g_strconcat("list:", job->name, NULL);
    g_hash_table_iter_init(&it, g_views);
    while (g_hash_table_iter_next(&it, NULL, &value)) {
        v = value;
        if (g_hash_table_contains(v->filter_lists, job->name) &&
            !g_hash_table_contains(v->lists_waiting, job->name))
            view_filter_load(v, key, id, 0, FALSE);
    }
    g_free(key);
    list_waiters_resolve(job->name, id);
    list_report(job->name, TRUE);
}

static void app_filter_compiled(FilterJob *job, const char *id, const char *why)
{
    KWView *v = view_lookup(job->profile, job->serial);
    char *key;

    if (!v)
        return;

    if (GPOINTER_TO_UINT(g_hash_table_lookup(v->filter_gen, job->name)) != job->generation) {
        g_message("view %s: content filter %s replaced while compiling", v->profile, job->name);
    } else if (!id) {
        key = g_strdup_printf("content filter %s: %s", job->name, why);
        view_config_fail(v, key);
        g_free(key);
    } else {
        key = g_strconcat(v->profile, "/", job->name, NULL);
        filter_index_set(key, id);
        g_free(key);
        key = g_strconcat("app:", job->name, NULL);
        view_filter_load(v, key, id, job->generation, TRUE);
        g_free(key);
    }
    view_pending_done(v);
}

static void filter_job_finish(FilterJob *job, const char *id, const char *why)
{
    if (job->profile)
        app_filter_compiled(job, id, why);
    else
        list_prepared(job, id, why);
}

static void filter_jobs_next(GAsyncReadyCallback done)
{
    GSubprocessFlags flags = G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE |
                             G_SUBPROCESS_FLAGS_STDERR_PIPE;
    GError *error = NULL;
    FilterJob *job;

    while (!g_filter_busy && (job = g_queue_pop_head(&g_filter_jobs))) {
        job->proc = g_subprocess_new(flags, &error, KW_FILTER_HELPER, g_filter_dir, NULL);
        if (!job->proc) {
            filter_job_finish(job, NULL, error->message);
            g_clear_error(&error);
            filter_job_free(job);
            continue;
        }
        g_filter_busy = TRUE;
        g_subprocess_communicate_async(job->proc, job->list, NULL, done, job);
    }
}

static char *filter_job_log(FilterJob *job, GBytes *err)
{
    char *last = NULL;
    char **lines;
    char *text;
    guint i;

    if (!err)
        return NULL;
    text = g_strndup(g_bytes_get_data(err, NULL), g_bytes_get_size(err));
    lines = g_strsplit(text, "\n", -1);
    for (i = 0; lines[i]; i++) {
        if (!*lines[i])
            continue;
        g_message("filter %s%s%s: %s", job->profile ? job->profile : "list",
                  job->profile ? "/" : " ", job->name, lines[i]);
        g_free(last);
        last = g_strdup(lines[i]);
    }
    g_strfreev(lines);
    g_free(text);
    return last;
}

static void filter_job_done(GObject *source, GAsyncResult *res, gpointer data)
{
    GSubprocess *proc = G_SUBPROCESS(source);
    FilterJob *job = data;
    GBytes *out = NULL;
    GBytes *err = NULL;
    char *id = NULL;
    char *why;

    if (!g_subprocess_communicate_finish(proc, res, &out, &err, NULL))
        why = g_strdup("the filter compiler could not be run");
    else
        why = filter_job_log(job, err);

    if (out && g_subprocess_get_if_exited(proc) && !g_subprocess_get_exit_status(proc))
        id = g_strstrip(g_strndup(g_bytes_get_data(out, NULL), g_bytes_get_size(out)));
    if (id && (strlen(id) != 64 || strspn(id, "0123456789abcdef") != 64))
        g_clear_pointer(&id, g_free);

    filter_job_finish(job, id, why ? why : "the filter compiler failed");

    g_free(id);
    g_free(why);
    if (out)
        g_bytes_unref(out);
    if (err)
        g_bytes_unref(err);
    filter_job_free(job);
    g_filter_busy = FALSE;
    filter_jobs_next(filter_job_done);
}

static void filter_job_queue(KWView *v, const char *name, guint generation, GBytes *list)
{
    FilterJob *job = g_new0(FilterJob, 1);

    job->profile = v ? g_strdup(v->profile) : NULL;
    job->serial = v ? v->serial : 0;
    job->name = g_strdup(name);
    job->generation = generation;
    job->list = list;
    g_queue_push_tail(&g_filter_jobs, job);
    filter_jobs_next(filter_job_done);
}

static void view_set_user_agent(void *data, struct kodi_web_view_v1 *proxy,
    const char *user_agent)
{
    KWView *v = data;

    (void)proxy;
    if (!user_agent || !*user_agent) {
        webkit_settings_set_user_agent(v->settings, NULL);
        return;
    }
    if (!ua_is_valid(user_agent)) {
        g_warning("view %s: rejecting invalid user agent, keeping current: \"%s\"",
            v->profile, user_agent);
        return;
    }
    webkit_settings_set_user_agent(v->settings, user_agent);
}

static void view_zoom_update(KWView *v)
{
    WPEView *view = webkit_web_view_get_wpe_view(v->web_view);
    double zoom = 1.0;
    int height;
    int width;

    width = view ? wpe_view_get_width(view) : 0;
    height = view ? wpe_view_get_height(view) : 0;
    if (v->viewport_width && v->viewport_height && width > 0 && height > 0)
        zoom = MIN((double)width / v->viewport_width, (double)height / v->viewport_height);
    webkit_web_view_set_zoom_level(v->web_view, zoom);
}

static void on_view_resized(WPEView *view, gpointer user_data)
{
    (void)view;
    view_zoom_update(user_data);
}

static void view_set_viewport(void *data, struct kodi_web_view_v1 *proxy, uint32_t width,
    uint32_t height)
{
    KWView *v = data;

    (void)proxy;
    v->viewport_width = width;
    v->viewport_height = height;
    view_zoom_update(v);
}

static void view_set_software_video_decoding(void *data, struct kodi_web_view_v1 *proxy,
    uint32_t allowed)
{
    KWView *v = data;

    (void)proxy;
    webkit_settings_set_media_content_types_requiring_hardware_support(v->settings,
        allowed ? NULL : KW_HARDWARE_VIDEO_TYPES);
}

static void view_add_user_script(void *data, struct kodi_web_view_v1 *proxy,
    const char *name, int32_t fd, uint32_t size)
{
    WebKitUserScript *script;
    KWView *v = data;
    char *src = NULL;
    gsize len = 0;
    gboolean ok;
    char *what;

    (void)proxy;
    ok = read_fd_exactly(fd, size, &src, &len);
    close(fd);
    if (!ok || len < size) {
        what = g_strdup_printf("user script %s: read %zu of %u bytes", name, len, size);
        view_config_fail(v, what);
        g_free(what);
        g_free(src);
        return;
    }

    what = g_strndup(src, len);
    script = webkit_user_script_new(what, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(v->ucm, script);
    webkit_user_script_unref(script);
    g_free(what);
    g_free(src);
}

static void view_add_content_filter(void *data, struct kodi_web_view_v1 *proxy,
    const char *name, int32_t fd, uint32_t size)
{
    KWView *v = data;
    char *src = NULL;
    guint generation;
    gsize len = 0;
    gboolean ok;
    char *what;

    (void)proxy;
    ok = read_fd_exactly(fd, size, &src, &len);
    close(fd);
    if (!id_valid(name) || !ok || len < size) {
        what = g_strdup_printf("content filter %s: invalid name or read %zu of %u bytes",
                               name, len, size);
        view_config_fail(v, what);
        g_free(what);
        g_free(src);
        return;
    }

    generation = GPOINTER_TO_UINT(g_hash_table_lookup(v->filter_gen, name)) + 1;
    g_hash_table_replace(v->filter_gen, g_strdup(name), GUINT_TO_POINTER(generation));
    v->pending++;
    filter_job_queue(v, name, generation, g_bytes_new_take(src, len));
}

static void view_add_content_filter_list(void *data, struct kodi_web_view_v1 *proxy,
    const char *name)
{
    KWView *v = data;
    const char *id;
    char *key;

    (void)proxy;
    if (!id_valid(name)) {
        g_warning("view %s: ignoring content filter list with invalid name", v->profile);
        return;
    }
    g_hash_table_add(v->filter_lists, g_strdup(name));

    id = g_hash_table_lookup(g_lists, name);
    if (!id) {
        if (g_hash_table_add(v->lists_waiting, g_strdup(name)))
            v->pending++;
        return;
    }
    key = g_strconcat("list:", name, NULL);
    view_filter_load(v, key, id, 0, TRUE);
    g_free(key);
}

static void view_load_url(void *data, struct kodi_web_view_v1 *proxy, const char *url)
{
    KWView *v = data;

    (void)proxy;
    v->exit_sent = FALSE;
    if (v->config_failed) {
        kodi_web_view_v1_error(v->proxy, KODI_WEB_VIEW_V1_ERROR_CODE_CONFIG_FAILED,
                               "the view's configuration could not be applied");
        kw_flush();
        return;
    }
    if (v->pending) {
        g_free(v->pending_url);
        v->pending_url = g_strdup(url ? url : "about:blank");
        return;
    }
    webkit_web_view_load_uri(v->web_view, url ? url : "about:blank");
}

static void view_navigate(void *data, struct kodi_web_view_v1 *proxy, uint32_t action)
{
    (void)proxy;
    KWView *v = data;
    switch (action) {
    case KODI_WEB_VIEW_V1_NAVIGATION_BACK:
        if (webkit_web_view_can_go_back(v->web_view))
            webkit_web_view_go_back(v->web_view);
        break;
    case KODI_WEB_VIEW_V1_NAVIGATION_FORWARD:
        if (webkit_web_view_can_go_forward(v->web_view))
            webkit_web_view_go_forward(v->web_view);
        break;
    case KODI_WEB_VIEW_V1_NAVIGATION_RELOAD:
        webkit_web_view_reload(v->web_view);
        break;
    case KODI_WEB_VIEW_V1_NAVIGATION_STOP:
        webkit_web_view_stop_loading(v->web_view);
        break;
    default:
        g_warning("view %s: unknown navigate action %u", v->profile, action);
        break;
    }
}

static void view_set_active(void *data, struct kodi_web_view_v1 *proxy, uint32_t active)
{
    (void)proxy;
    KWView *v = data;
    v->active = active ? TRUE : FALSE;
    /*
     * An inactive view keeps its page alive (media keeps playing, scripts
     * keep running) with no action needed: WebKit only paints on content
     * change, so the "must still commit a changed frame" invariant holds by
     * default. WPE 2.54.0 exposes no public per-view render-throttle on the
     * WebKitWebView / WPEView, so no explicit frequency change is made here.
     */
}

static void view_close(void *data, struct kodi_web_view_v1 *proxy)
{
    (void)proxy;
    KWView *v = data;
    webkit_web_view_load_uri(v->web_view, "about:blank");  /* unload, stop media */
    view_destroy(v, TRUE);
}

static const struct kodi_web_view_v1_listener view_listener = {
    view_set_user_agent,
    view_add_user_script,
    view_add_content_filter,
    view_add_content_filter_list,
    view_load_url,
    view_navigate,
    view_set_active,
    view_close,
    view_set_viewport,
    view_set_software_video_decoding
};

/* ------------------------------------------------------------------ */
/* View lifecycle                                                      */
/* ------------------------------------------------------------------ */

static void view_destroy(KWView *v, gboolean send_protocol_destroy)
{
    if (!v)
        return;

    if (v->cancellable)
        g_cancellable_cancel(v->cancellable);

    if (send_protocol_destroy && v->proxy) {
        kodi_web_view_v1_destroy(v->proxy);  /* destructor: frees the proxy */
        v->proxy = NULL;
        kw_flush();
    }

    g_hash_table_remove(g_views, v->profile);  /* value-destroy frees the rest */
}

static void view_free(gpointer data)
{
    KWView *v = data;
    if (v->favicon_retry_id)
        g_source_remove(v->favicon_retry_id);
    if (v->proxy)
        kodi_web_view_v1_destroy(v->proxy);
    if (v->web_view)
        g_signal_handlers_disconnect_by_data(webkit_web_view_get_wpe_view(v->web_view), v);
    g_clear_object(&v->web_view);
    g_clear_object(&v->session);
    g_clear_object(&v->settings);
    if (v->ucm)
        g_signal_handlers_disconnect_by_data(v->ucm, v);
    g_clear_object(&v->ucm);
    if (v->cancellable)
        g_object_unref(v->cancellable);
    g_hash_table_destroy(v->filters);
    g_hash_table_destroy(v->filter_gen);
    g_hash_table_destroy(v->filter_lists);
    g_hash_table_destroy(v->lists_waiting);
    g_hash_table_destroy(v->media_frames);
    g_free(v->last_favicon_hash);
    g_free(v->current_url);
    g_free(v->pending_url);
    g_free(v->failed_uri);
    g_free(v->profile);
    g_free(v);
}

static gboolean session_property_exists(const char *name)
{
    GObjectClass *klass;
    gboolean found;

    klass = g_type_class_ref(WEBKIT_TYPE_NETWORK_SESSION);
    found = g_object_class_find_property(klass, name) != NULL;
    g_type_class_unref(klass);
    return found;
}

static void kw_choose_disk_cache(void)
{
    if (g_strcmp0(g_getenv("KODI_WEBVIEW_DISK_CACHE"), "0"))
        return;
    if (!session_property_exists("disk-cache-enabled")) {
        g_warning("the engine cannot keep HTTP resources in memory only, caching them on disk");
        return;
    }
    g_disk_cache = FALSE;
    g_message("HTTP resources are cached in memory only");
}

static void session_property_set_string(const char **names, GValue *values, guint *n,
    const char *name, const char *value)
{
    names[*n] = name;
    g_value_init(&values[*n], G_TYPE_STRING);
    g_value_set_string(&values[*n], value);
    (*n)++;
}

static void session_property_set_boolean(const char **names, GValue *values, guint *n,
    const char *name, gboolean value)
{
    names[*n] = name;
    g_value_init(&values[*n], G_TYPE_BOOLEAN);
    g_value_set_boolean(&values[*n], value);
    (*n)++;
}

/* A data_dir of NULL makes the session ephemeral. */
static WebKitNetworkSession *session_create(const char *data_dir, const char *cache_dir,
    const char *uplink, char **why)
{
    GValue values[KW_SESSION_PROPERTIES_MAX] = { G_VALUE_INIT };
    const char *names[KW_SESSION_PROPERTIES_MAX];
    WebKitNetworkSession *session;
    guint n = 0;
    guint i;

    if (uplink && *uplink && !session_property_exists("uplink")) {
        *why = g_strdup_printf("the engine cannot bind connections to uplink '%s'", uplink);
        return NULL;
    }

    if (!data_dir) {
        session_property_set_boolean(names, values, &n, "is-ephemeral", TRUE);
    } else {
        session_property_set_string(names, values, &n, "data-directory", data_dir);
        session_property_set_string(names, values, &n, "cache-directory", cache_dir);
        if (!g_disk_cache)
            session_property_set_boolean(names, values, &n, "disk-cache-enabled", FALSE);
    }
    if (uplink && *uplink) {
        g_message("network session bound to uplink '%s'", uplink);
        session_property_set_string(names, values, &n, "uplink", uplink);
    }

    session = WEBKIT_NETWORK_SESSION(g_object_new_with_properties(WEBKIT_TYPE_NETWORK_SESSION,
                                                                  n, names, values));
    for (i = 0; i < n; i++)
        g_value_unset(&values[i]);
    return session;
}

static gboolean view_storage_create(KWView *v, const char *uplink, char **why)
{
    WebKitCookieManager *cm;
    char *cache_dir;
    char *data_dir;
    char *cookies;

    if (v->ephemeral) {
        v->session = session_create(NULL, NULL, uplink, why);
        return v->session != NULL;
    }

    data_dir = g_build_filename(g_get_user_data_dir(), "kodi-webview", v->profile, NULL);
    cache_dir = g_build_filename(g_get_user_cache_dir(), "kodi-webview", v->profile, NULL);
    cookies = g_build_filename(data_dir, "cookies.db", NULL);

    if (g_mkdir_with_parents(data_dir, 0700) || g_mkdir_with_parents(cache_dir, 0700))
        *why = g_strdup_printf("cannot create profile storage under %s: %s", data_dir,
                               g_strerror(errno));
    else
        v->session = session_create(data_dir, cache_dir, uplink, why);

    if (v->session) {
        cm = webkit_network_session_get_cookie_manager(v->session);
        webkit_cookie_manager_set_persistent_storage(cm, cookies,
                                                     WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
    }

    g_free(data_dir);
    g_free(cache_dir);
    g_free(cookies);
    return v->session != NULL;
}

static void view_scripts_add(KWView *v)
{
    WebKitUserScript *script;

    g_signal_connect(v->ucm, "script-message-received::kodi", G_CALLBACK(on_exit_message), v);
    g_signal_connect(v->ucm, "script-message-received::kodimedia",
                     G_CALLBACK(on_media_message), v);
    webkit_user_content_manager_register_script_message_handler(v->ucm, "kodi", NULL);
    webkit_user_content_manager_register_script_message_handler(v->ucm, "kodimedia",
                                                                KW_MEDIA_WORLD);

    script = webkit_user_script_new(EXIT_SCRIPT, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                    WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START, NULL, NULL);
    webkit_user_content_manager_add_script(v->ucm, script);
    webkit_user_script_unref(script);

    script = webkit_user_script_new_for_world(MEDIA_SCRIPT, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                              WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                              KW_MEDIA_WORLD, NULL, NULL);
    webkit_user_content_manager_add_script(v->ucm, script);
    webkit_user_script_unref(script);
}

/*
 * Build the view: network session (ephemeral for "default", persistent
 * otherwise, bound to the uplink when one is given), settings, surface, and
 * the protocol proxy via create_view. Returns NULL with *why set on failure.
 * Does NOT load any URL.
 */
static KWView *view_create(const char *profile, const char *uplink, char **why)
{
    *why = NULL;

    if (!id_valid(profile)) {
        *why = g_strdup("invalid profile identifier");
        return NULL;
    }
    if (g_hash_table_lookup(g_views, profile)) {
        *why = g_strdup_printf("a view for profile '%s' already exists", profile);
        return NULL;
    }

    KWView *v = g_new0(KWView, 1);
    v->profile = g_strdup(profile);
    v->serial = ++g_view_serial;
    v->ephemeral = (g_strcmp0(v->profile, "default") == 0);
    v->active = TRUE;
    v->filters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    v->filter_gen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    v->filter_lists = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    v->lists_waiting = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    v->media_frames = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    v->cancellable = g_cancellable_new();

    if (!view_storage_create(v, uplink, why)) {
        view_free(v);
        return NULL;
    }

    /* Enable website icons on this view's session (off by default) so the
     * favicon database populates; get its handle for later fetches. */
    WebKitWebsiteDataManager *wdm = webkit_network_session_get_website_data_manager(v->session);
    webkit_website_data_manager_set_favicons_enabled(wdm, TRUE);
    v->favicon_db = webkit_website_data_manager_get_favicon_database(wdm);
    g_message("favicon db: %s enabled=%d", v->profile, v->favicon_db ? 1 : 0);

    v->ucm = webkit_user_content_manager_new();
    view_scripts_add(v);

    /* No user agent is set: the view uses the engine default until a valid
     * set_user_agent event arrives (per the protocol). */
    v->settings = webkit_settings_new_with_settings(
        "enable-spatial-navigation", TRUE,
        "enable-mediasource", TRUE,
        "media-playback-requires-user-gesture", FALSE,
        "enable-encrypted-media", TRUE,
        "media-content-types-requiring-hardware-support", KW_HARDWARE_VIDEO_TYPES,
        NULL);

    WebKitWebsitePolicies *policies = webkit_website_policies_new_with_policies(
        "autoplay", WEBKIT_AUTOPLAY_ALLOW, NULL);

    v->web_view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW,
        "network-session", v->session,
        "settings", v->settings,
        "website-policies", policies,
        "user-content-manager", v->ucm,
        "display", g_display,
        NULL));
    g_object_unref(policies);
    if (g_video_plane)
        webkit_web_view_set_background_color(v->web_view, &g_transparent);

    /* Obtain the wl_surface backing the view to hand to create_view. */
    WPEView *wpe_view = webkit_web_view_get_wpe_view(v->web_view);
    struct wl_surface *surface = NULL;
    if (wpe_view && WPE_IS_VIEW_WAYLAND(wpe_view))
        surface = wpe_view_wayland_get_wl_surface(WPE_VIEW_WAYLAND(wpe_view));
    if (!surface) {
        *why = g_strdup("could not obtain the view's wl_surface from WPE");
        view_free(v);
        return NULL;
    }

    /* The verbatim profile id is sent on the wire. */
    v->proxy = kodi_web_manager_v1_create_view(g_manager, v->profile, surface);
    if (!v->proxy) {
        *why = g_strdup("create_view request failed");
        view_free(v);
        return NULL;
    }
    kodi_web_view_v1_add_listener(v->proxy, &view_listener, v);

    /* View state reports. */
    g_signal_connect(v->web_view, "notify::uri", G_CALLBACK(on_notify_uri), v);
    g_signal_connect(v->web_view, "notify::title", G_CALLBACK(on_notify_title), v);
    g_signal_connect(v->web_view, "notify::estimated-load-progress",
        G_CALLBACK(on_notify_progress), v);
    g_signal_connect(v->web_view, "load-changed", G_CALLBACK(on_load_changed), v);
    g_signal_connect(v->web_view, "load-failed", G_CALLBACK(on_load_failed), v);
    g_signal_connect(v->web_view, "load-failed-with-tls-errors",
        G_CALLBACK(on_load_failed_tls), v);
    g_signal_connect(v->web_view, "web-process-terminated",
        G_CALLBACK(on_web_process_terminated), v);
    g_signal_connect(v->web_view, "permission-request",
        G_CALLBACK(on_permission_request), v);
    g_signal_connect(v->web_view, "notify::is-playing-audio",
                     G_CALLBACK(on_notify_playing_audio), v);
    g_signal_connect(v->web_view, "close", G_CALLBACK(on_close), v);
    g_signal_connect(wpe_view, "resized", G_CALLBACK(on_view_resized), v);

    g_hash_table_insert(g_views, g_strdup(v->profile), v);
    kw_flush();
    return v;
}

/* ------------------------------------------------------------------ */
/* Manager events (compositor -> client)                               */
/* ------------------------------------------------------------------ */

static void manager_want_view(void *data, struct kodi_web_manager_v1 *manager,
    const char *profile, const char *uplink)
{
    char *why = NULL;
    KWView *v;

    (void)data;
    v = view_create(profile, uplink, &why);
    if (!v) {
        kodi_web_manager_v1_view_unavailable(manager, profile,
            why ? why : "view could not be created");
        kw_flush();
    }
    g_free(why);
    /* No URL is loaded: configuration events then load_url will arrive. */
}

static void manager_content_filter_list(void *data, struct kodi_web_manager_v1 *manager,
    const char *name, int32_t fd, uint32_t size)
{
    char *src = NULL;
    guint generation;
    gsize len = 0;
    char *cached;
    gboolean ok;

    (void)data;
    (void)manager;
    ok = read_fd_exactly(fd, size, &src, &len);
    close(fd);
    if (!id_valid(name) || !ok || len < size) {
        g_warning("content filter list %s: invalid name or read %zu of %u bytes", name, len,
                  size);
        g_free(src);
        return;
    }

    if (g_hash_table_steal_extended(g_lists_cached, name, NULL, (gpointer *)&cached)) {
        g_hash_table_replace(g_lists, g_strdup(name), cached);
        list_waiters_resolve(name, cached);
        list_report(name, TRUE);
    }

    generation = GPOINTER_TO_UINT(g_hash_table_lookup(g_list_gen, name)) + 1;
    g_hash_table_replace(g_list_gen, g_strdup(name), GUINT_TO_POINTER(generation));
    filter_job_queue(NULL, name, generation, g_bytes_new_take(src, len));
}

static void manager_remove_content_filter_list(void *data, struct kodi_web_manager_v1 *manager,
    const char *name)
{
    GHashTableIter it;
    guint generation;
    gpointer value;
    char *key;

    (void)data;
    (void)manager;
    generation = GPOINTER_TO_UINT(g_hash_table_lookup(g_list_gen, name)) + 1;
    g_hash_table_replace(g_list_gen, g_strdup(name), GUINT_TO_POINTER(generation));
    g_hash_table_remove(g_lists, name);
    g_hash_table_remove(g_lists_cached, name);
    list_waiters_resolve(name, NULL);

    key = g_strconcat("@list/", name, NULL);
    filter_index_set(key, NULL);
    g_free(key);

    key = g_strconcat("list:", name, NULL);
    g_hash_table_iter_init(&it, g_views);
    while (g_hash_table_iter_next(&it, NULL, &value))
        view_filter_replace(value, key, NULL);
    g_free(key);
}

static const struct kodi_web_manager_v1_listener manager_listener = {
    manager_want_view,
    manager_content_filter_list,
    manager_remove_content_filter_list
};

/* ------------------------------------------------------------------ */
/* Registry (our own, on WPE's wl_display)                             */
/* ------------------------------------------------------------------ */

static void registry_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    if (g_strcmp0(interface, kodi_web_manager_v1_interface.name) == 0 && !g_manager) {
        g_manager = wl_registry_bind(registry, name, &kodi_web_manager_v1_interface,
                                     MIN(version, 4));
        g_manager_name = name;
        kodi_web_manager_v1_add_listener(g_manager, &manager_listener, NULL);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data;
    (void)registry;
    if (g_manager && name == g_manager_name) {
        g_warning("kodi_web_manager_v1 global removed by compositor");
        if (g_app)
            g_application_quit(g_app);
    }
}

static const struct wl_registry_listener registry_listener = {
    registry_global,
    registry_global_remove
};

/* ------------------------------------------------------------------ */
/* Application                                                         */
/* ------------------------------------------------------------------ */

static void on_display_disconnected(WPEDisplay *display, GError *error, gpointer user_data)
{
    (void)display;
    (void)user_data;
    if (error)
        g_warning("WPE display disconnected: %s", error->message);
    else
        g_warning("WPE display disconnected");
    _exit(1);
}

static void activate(GApplication *application, gpointer user_data)
{
    WPEDisplayWayland *wayland;
    GError *error = NULL;
    (void)user_data;
    g_application_hold(application);

    /* 1. Connect over the inherited Wayland socket (WAYLAND_SOCKET fd). */
    wayland = WPE_DISPLAY_WAYLAND(wpe_display_wayland_new());
    if (!wpe_display_wayland_connect(wayland, NULL, &error)) {
        g_printerr("kodi-webview: cannot connect to Wayland display: %s\n",
            error ? error->message : "unknown error");
        g_clear_error(&error);
        g_application_quit(application);
        return;
    }
    g_display = WPE_DISPLAY(wayland);
    wpe_display_set_primary(g_display);
    g_signal_connect(g_display, "disconnected", G_CALLBACK(on_display_disconnected), NULL);

    /* 2. Bind kodi_web_manager_v1 using our OWN registry + roundtrip on
     * WPE's wl_display, without touching WPE's own global handling. */
    g_wl_display = wpe_display_wayland_get_wl_display(wayland);
    if (!g_wl_display) {
        g_printerr("kodi-webview: WPE returned no wl_display\n");
        g_application_quit(application);
        return;
    }
    g_views = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, view_free);
    kw_choose_disk_cache();
    filter_store_open();

    g_registry = wl_display_get_registry(g_wl_display);
    wl_registry_add_listener(g_registry, &registry_listener, NULL);
    wl_display_roundtrip(g_wl_display);   /* receive and bind the manager */

    if (!g_manager) {
        g_printerr("kodi-webview: compositor does not offer kodi_web_manager_v1\n");
        g_application_quit(application);
        return;
    }

    kodi_web_manager_v1_ready(g_manager);
    kw_flush();
}

static gboolean quit_on_signal(gpointer app)
{
    g_application_quit(G_APPLICATION(app));
    return G_SOURCE_REMOVE;
}

/* Above every system audio sink, pulsesink being PRIMARY + 10. */
#define KW_AUDIO_SINK_RANK (GST_RANK_PRIMARY + 100)

static void kw_route_audio_to_kodi(void)
{
    const char *path = g_getenv(KODI_WEB_AUDIO_ENV);
    const char *existing;
    char *rank;

    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_message("audio: no Kodi audio socket, pages use the system audio sink");
        return;
    }

    webkit_web_context_add_path_to_sandbox(webkit_web_context_get_default(),
        path, FALSE);

    existing = g_getenv("GST_PLUGIN_FEATURE_RANK");
    if (existing && *existing)
        rank = g_strdup_printf("%s,kodiaudiosink:%d", existing, KW_AUDIO_SINK_RANK);
    else
        rank = g_strdup_printf("kodiaudiosink:%d", KW_AUDIO_SINK_RANK);
    g_setenv("GST_PLUGIN_FEATURE_RANK", rank, TRUE);
    g_free(rank);

    g_message("audio: pages play through Kodi at %s", path);
}

static void kw_route_video_to_kodi(void)
{
    const char *path = g_getenv(KODI_WEB_VIDEO_ENV);

    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) {
        g_message("video: no Kodi video socket, pages draw their video");
        return;
    }

    webkit_web_context_add_path_to_sandbox(webkit_web_context_get_default(),
        path, FALSE);
    g_setenv("WEBKIT_GST_HOLE_PUNCH_QUIRK", "kodi", TRUE);
    g_video_plane = TRUE;
    g_message("video: pages show their video on Kodi's plane at %s", path);
}

/* The engine binds and loads the CDM named by WPE_WIDEVINE_CDM_PATH. */
static void kw_offer_widevine(void)
{
    const char *dir = g_getenv("KODI_WEB_CDM_DIR");
    char *path;

    if (!dir || g_getenv("WPE_WIDEVINE_CDM_PATH"))
        return;

    path = g_build_filename(dir, "libwidevinecdm.so", NULL);
    if (g_file_test(path, G_FILE_TEST_IS_REGULAR)) {
        g_setenv("WPE_WIDEVINE_CDM_PATH", path, TRUE);
        g_message("drm: pages decrypt with %s", path);
    } else {
        g_message("drm: no Widevine CDM in %s", dir);
    }
    g_free(path);
}

#ifndef KW_WIDEVINE_PRELOAD
#define KW_WIDEVINE_PRELOAD ""
#endif

static void kw_preload_drop(const char *library)
{
    const char *preload = g_getenv("LD_PRELOAD");
    GString *kept;
    char **entries;
    int i;

    if (!preload)
        return;

    entries = g_strsplit_set(preload, " :", -1);
    kept = g_string_new(NULL);
    for (i = 0; entries[i]; i++) {
        if (!*entries[i] || !strcmp(entries[i], library))
            continue;
        if (kept->len)
            g_string_append_c(kept, ':');
        g_string_append(kept, entries[i]);
    }
    if (kept->len)
        g_setenv("LD_PRELOAD", kept->str, TRUE);
    else
        g_unsetenv("LD_PRELOAD");
    g_string_free(kept, TRUE);
    g_strfreev(entries);
}

/* Kodi starts with the library its own CDM loader needs; of our children
 * only the web process loads the CDM, and the engine preloads it there. */
static void kw_preload_for_widevine(void)
{
    if (!*KW_WIDEVINE_PRELOAD)
        return;

    kw_preload_drop(KW_WIDEVINE_PRELOAD);
    if (g_getenv("WPE_WIDEVINE_CDM_PATH"))
        g_setenv("WPE_WIDEVINE_CDM_PRELOAD", KW_WIDEVINE_PRELOAD, FALSE);
}

static void kw_enable_adaptive_streaming(void)
{
    g_setenv("WEBKIT_GST_ENABLE_DASH_SUPPORT", "1", FALSE);
}

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Decoder-rank policy must be in place before the engine's media
     * pipeline (and the child processes that run it) start. */
    kw_apply_codec_policy();
    kw_route_audio_to_kodi();
    kw_route_video_to_kodi();
    kw_offer_widevine();
    kw_preload_for_widevine();
    kw_enable_adaptive_streaming();

    g_app = g_application_new("org.kodi.webview", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(g_app, "activate", G_CALLBACK(activate), NULL);

    g_unix_signal_add(SIGINT, quit_on_signal, g_app);
    g_unix_signal_add(SIGTERM, quit_on_signal, g_app);

    int status = g_application_run(g_app, 0, NULL);

    /* the compositor sends the lists in use at some point during a run */
    if (g_lists_cached)
        filter_lists_forget_cached();
    if (g_views)
        g_hash_table_destroy(g_views);
    g_object_unref(g_app);
    return status;
}
