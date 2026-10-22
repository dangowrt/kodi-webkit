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
 * load_progress, navigation_state, error, destroy) are client-to-compositor
 * REQUESTS; the compositor's commands (want_view on the manager;
 * set_user_agent, add_user_script, add_content_filter, load_url, navigate,
 * set_active, close on the view) are EVENTS this client handles.
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

/* Fixed, greppable prefix for the ephemeral default view's throwaway content
 * filter store under g_get_tmp_dir(); used both to create and to reap it. */
#define KW_DEFAULT_TMP_PREFIX "kodi-webview-default-"

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
static gboolean g_ready_sent = FALSE;

/* ------------------------------------------------------------------ */
/* Per-view state                                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char *profile;                  /* verbatim protocol id (may carry dots) */
    char *dir;                      /* filesystem-safe directory name        */
    gboolean is_default;            /* the warm default view                 */
    gboolean ephemeral;             /* profile "default": no on-disk state    */
    char *tmp_dir;                  /* throwaway dir for ephemeral filter store */

    WebKitNetworkSession *session;
    WebKitSettings *settings;
    WebKitUserContentManager *ucm;
    WebKitUserContentFilterStore *filter_store;
    WebKitFaviconDatabase *favicon_db;
    WebKitWebView *web_view;

    struct kodi_web_view_v1 *proxy;

    GCancellable *cancellable;      /* cancels pending async on close         */
    GHashTable *filter_hashes;      /* name (char*) -> sha256 hex (char*)     */
    char *last_favicon_hash;        /* sha256 of the last favicon sent        */
    guint favicon_retry_id;         /* repeating poll source after a load     */
    int favicon_tries;              /* icon-fetch attempts for the current load */
    gboolean favicon_done;          /* an icon was delivered for this load    */

    char *current_url;              /* last committed URL, for recovery       */
    gboolean active;
    gboolean crash_recovering;      /* set on crash, cleared on next load     */
    gboolean reload_started;        /* recovery reload has begun loading      */
} KWView;

static void kw_flush(void)
{
    if (g_wl_display)
        wl_display_flush(g_wl_display);
}

/* ------------------------------------------------------------------ */
/* GStreamer decoder-rank policy (KODI_WEBVIEW_SOFTWARE_CODECS)         */
/*                                                                     */
/* With 0 (default) we demote every software VIDEO decoder whose codec */
/* has no hardware decoder, so the engine offers only codecs the box   */
/* can decode in hardware. Ranks are process-global, so this is set    */
/* both in this process (gst_plugin_feature_set_rank) and in the       */
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

static void kw_apply_codec_policy(void)
{
    const char *sw = g_getenv("KODI_WEBVIEW_SOFTWARE_CODECS");
    if (sw && g_strcmp0(sw, "1") == 0) {
        g_message("codec policy: KODI_WEBVIEW_SOFTWARE_CODECS=1, leaving decoder ranks untouched");
        return;
    }

    gst_init(NULL, NULL);
    GstRegistry *registry = gst_registry_get();
    GList *features = gst_registry_get_feature_list(registry, GST_TYPE_ELEMENT_FACTORY);

    /* Pass 1: codecs with a hardware decoder. */
    GHashTable *hw_codecs = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (GList *l = features; l; l = l->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(l->data);
        gboolean is_hw = FALSE;
        if (!factory_is_video_decoder(factory, &is_hw) || !is_hw)
            continue;
        GPtrArray *types = factory_sink_video_types(factory);
        for (guint i = 0; i < types->len; i++)
            g_hash_table_add(hw_codecs, g_strdup(g_ptr_array_index(types, i)));
        g_ptr_array_unref(types);
    }

    /* Pass 2: demote software video decoders whose codecs have no hardware. */
    GString *env = g_string_new(NULL);
    GPtrArray *demoted = g_ptr_array_new_with_free_func(g_free);
    for (GList *l = features; l; l = l->next) {
        GstElementFactory *factory = GST_ELEMENT_FACTORY(l->data);
        gboolean is_hw = FALSE;
        if (!factory_is_video_decoder(factory, &is_hw) || is_hw)
            continue;
        GPtrArray *types = factory_sink_video_types(factory);
        gboolean has_hw = FALSE;
        for (guint i = 0; i < types->len; i++)
            if (g_hash_table_contains(hw_codecs, g_ptr_array_index(types, i))) { has_hw = TRUE; break; }
        g_ptr_array_unref(types);
        if (has_hw || types->len == 0)
            continue;

        gst_plugin_feature_set_rank(GST_PLUGIN_FEATURE(factory), GST_RANK_NONE);
        const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
        g_ptr_array_add(demoted, g_strdup(name));
        if (env->len)
            g_string_append_c(env, ',');
        g_string_append_printf(env, "%s:NONE", name);
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
        g_message("codec policy: no software-only video decoders to demote");
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

static void on_notify_uri(WebKitWebView *view, GParamSpec *pspec, gpointer user_data)
{
    (void)pspec;
    KWView *v = user_data;
    const char *uri = webkit_web_view_get_uri(view);
    g_free(v->current_url);
    v->current_url = g_strdup(uri ? uri : "");
    kodi_web_view_v1_url_changed(v->proxy, uri ? uri : "");
    kw_flush();
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
    }

    if (event == WEBKIT_LOAD_COMMITTED || event == WEBKIT_LOAD_FINISHED) {
        kodi_web_view_v1_navigation_state(v->proxy,
            webkit_web_view_can_go_back(view) ? 1 : 0,
            webkit_web_view_can_go_forward(view) ? 1 : 0);
    }

    if (event == WEBKIT_LOAD_FINISHED) {
        v->crash_recovering = FALSE;     /* a load completed: recovery ok  */
        v->reload_started = FALSE;

        /* No favicon-change signal in this build: poll the favicon database
         * from here (the icon may register slightly after load). */
        favicon_start_poll(v);

        if (v->is_default && !g_ready_sent && g_manager) {
            /* Engine processes are up and the warm default view exists. */
            kodi_web_manager_v1_ready(g_manager);
            g_ready_sent = TRUE;
        }
    }
    kw_flush();
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent event,
    const char *failing_uri, GError *error, gpointer user_data)
{
    (void)view;
    (void)event;
    (void)failing_uri;
    KWView *v = user_data;
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

/* Best-effort recursive removal of a directory tree. */
static void kw_rmrf(const char *path)
{
    GDir *d = g_dir_open(path, 0, NULL);
    if (d) {
        const char *name;
        while ((name = g_dir_read_name(d))) {
            char *child = g_build_filename(path, name, NULL);
            if (g_file_test(child, G_FILE_TEST_IS_DIR) && !g_file_test(child, G_FILE_TEST_IS_SYMLINK))
                kw_rmrf(child);
            else
                g_unlink(child);
            g_free(child);
        }
        g_dir_close(d);
    }
    g_rmdir(path);
}

/* Remove every ephemeral default view's throwaway store. Used on the
 * disconnect/exit path, which ends in _exit() and so runs no clean teardown. */
static void cleanup_temp_dirs(void)
{
    if (!g_views)
        return;
    GHashTableIter it;
    gpointer key, val;
    g_hash_table_iter_init(&it, g_views);
    while (g_hash_table_iter_next(&it, &key, &val)) {
        KWView *v = val;
        if (v->tmp_dir)
            kw_rmrf(v->tmp_dir);
    }
}

/* Reap throwaway default stores left behind by a previous run (for example a
 * client killed, or one that exited through the Wayland disconnect path). */
static void reap_stale_default_stores(void)
{
    const char *tmp = g_get_tmp_dir();
    GDir *d = g_dir_open(tmp, 0, NULL);
    if (!d)
        return;
    const char *name;
    while ((name = g_dir_read_name(d))) {
        if (!g_str_has_prefix(name, KW_DEFAULT_TMP_PREFIX))
            continue;
        char *p = g_build_filename(tmp, name, NULL);
        if (g_file_test(p, G_FILE_TEST_IS_DIR) && !g_file_test(p, G_FILE_TEST_IS_SYMLINK))
            kw_rmrf(p);
        g_free(p);
    }
    g_dir_close(d);
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

static void view_set_user_agent(void *data, struct kodi_web_view_v1 *proxy,
    const char *user_agent)
{
    (void)proxy;
    KWView *v = data;
    if (!user_agent || !*user_agent) {
        webkit_settings_set_user_agent(v->settings, NULL);  /* reset to default */
        return;
    }
    if (!ua_is_valid(user_agent)) {
        /* A malformed UA trips WebCore::isValidUserAgentHeaderValue; keep the
         * current one rather than take a critical. */
        g_warning("view %s: rejecting invalid user agent, keeping current: \"%s\"",
            v->profile, user_agent);
        return;
    }
    webkit_settings_set_user_agent(v->settings, user_agent);
}

static void view_add_user_script(void *data, struct kodi_web_view_v1 *proxy,
    const char *name, int32_t fd, uint32_t size)
{
    (void)proxy;
    KWView *v = data;
    char *src = NULL;
    gsize len = 0;
    gboolean ok = read_fd_exactly(fd, size, &src, &len);
    close(fd);
    if (!ok || !src) {
        g_warning("user script %s: read failed", name ? name : "(unnamed)");
        g_free(src);
        return;
    }
    if (len < size)
        g_warning("user script %s: short read (%zu of %u bytes)", name ? name : "(unnamed)",
            (size_t)len, size);
    char *nul = g_strndup(src, len);
    g_free(src);

    WebKitUserScript *script = webkit_user_script_new(nul,
        WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
        WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
        NULL, NULL);
    g_free(nul);
    webkit_user_content_manager_add_script(v->ucm, script);  /* arrival order */
    webkit_user_script_unref(script);
}

typedef struct {
    KWView *view;
    char *name;
    char *hash;
} FilterSaveCtx;

static void on_filter_saved(GObject *source, GAsyncResult *res, gpointer user_data)
{
    FilterSaveCtx *ctx = user_data;
    GError *error = NULL;
    WebKitUserContentFilter *filter = webkit_user_content_filter_store_save_finish(
        WEBKIT_USER_CONTENT_FILTER_STORE(source), res, &error);
    if (!filter) {
        if (!error || !g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            g_warning("content filter %s compile/store failed: %s", ctx->name,
                error ? error->message : "(no message)");
        g_clear_error(&error);
        g_free(ctx->name);
        g_free(ctx->hash);
        g_free(ctx);
        return;
    }
    /* Replace any earlier filter under this name, then add the new one. */
    webkit_user_content_manager_remove_filter_by_id(ctx->view->ucm, ctx->name);
    webkit_user_content_manager_add_filter(ctx->view->ucm, filter);
    webkit_user_content_filter_unref(filter);

    g_hash_table_replace(ctx->view->filter_hashes, g_strdup(ctx->name), g_strdup(ctx->hash));

    g_free(ctx->name);
    g_free(ctx->hash);
    g_free(ctx);
}

static void view_add_content_filter(void *data, struct kodi_web_view_v1 *proxy,
    const char *name, int32_t fd, uint32_t size)
{
    (void)proxy;
    KWView *v = data;
    if (!v->filter_store) {
        /* No store (ephemeral default without a temp dir): nothing to do. */
        close(fd);
        g_warning("view %s: no content-filter store, ignoring filter %s",
            v->profile, name ? name : "(unnamed)");
        return;
    }
    char *src = NULL;
    gsize len = 0;
    gboolean ok = read_fd_exactly(fd, size, &src, &len);
    close(fd);
    if (!ok || !src) {
        g_warning("content filter %s: read failed", name ? name : "(unnamed)");
        g_free(src);
        return;
    }

    char *hash = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)src, len);
    const char *prev = g_hash_table_lookup(v->filter_hashes, name);
    if (prev && g_strcmp0(prev, hash) == 0) {
        /* Identical resend: compiled form already stored and added. */
        g_free(src);
        g_free(hash);
        return;
    }

    GBytes *bytes = g_bytes_new_take(src, len);  /* takes ownership of src */
    FilterSaveCtx *ctx = g_new0(FilterSaveCtx, 1);
    ctx->view = v;
    ctx->name = g_strdup(name);
    ctx->hash = hash;   /* transferred */
    webkit_user_content_filter_store_save(v->filter_store, name, bytes,
        v->cancellable, on_filter_saved, ctx);
    g_bytes_unref(bytes);
}

static void view_load_url(void *data, struct kodi_web_view_v1 *proxy, const char *url)
{
    (void)proxy;
    KWView *v = data;
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
    view_load_url,
    view_navigate,
    view_set_active,
    view_close
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
    g_clear_object(&v->web_view);
    g_clear_object(&v->session);
    g_clear_object(&v->settings);
    g_clear_object(&v->ucm);
    g_clear_object(&v->filter_store);
    if (v->cancellable)
        g_object_unref(v->cancellable);
    if (v->filter_hashes)
        g_hash_table_destroy(v->filter_hashes);
    if (v->tmp_dir)
        kw_rmrf(v->tmp_dir);   /* throwaway store for the ephemeral default */
    g_free(v->last_favicon_hash);
    g_free(v->current_url);
    g_free(v->tmp_dir);
    g_free(v->dir);
    g_free(v->profile);
    g_free(v);

    /* The browser exits when no views remain. */
    if (g_views && g_hash_table_size(g_views) == 0 && g_app)
        g_application_quit(g_app);
}

/* A data_dir of NULL makes the session ephemeral. */
static WebKitNetworkSession *session_create(const char *data_dir, const char *cache_dir,
    const char *uplink, char **why)
{
    GObjectClass *klass;
    gboolean supported;

    if (!uplink || !*uplink)
        return data_dir ? webkit_network_session_new(data_dir, cache_dir)
                        : webkit_network_session_new_ephemeral();

    klass = g_type_class_ref(WEBKIT_TYPE_NETWORK_SESSION);
    supported = g_object_class_find_property(klass, "uplink") != NULL;
    g_type_class_unref(klass);
    if (!supported) {
        *why = g_strdup_printf("the engine cannot bind connections to uplink '%s'", uplink);
        return NULL;
    }

    g_message("network session bound to uplink '%s'", uplink);
    if (!data_dir)
        return WEBKIT_NETWORK_SESSION(g_object_new(WEBKIT_TYPE_NETWORK_SESSION,
            "is-ephemeral", TRUE, "uplink", uplink, NULL));
    return WEBKIT_NETWORK_SESSION(g_object_new(WEBKIT_TYPE_NETWORK_SESSION,
        "data-directory", data_dir, "cache-directory", cache_dir, "uplink", uplink, NULL));
}

/* Derive a filesystem-safe directory name from a verbatim profile id. */
static char *profile_to_dir(const char *profile)
{
    if (!profile || !*profile)
        return g_strdup("default");
    GString *s = g_string_new(NULL);
    for (const char *c = profile; *c; c++)
        g_string_append_c(s, g_ascii_isalnum(*c) ? *c : '-');
    if (!s->len)
        g_string_append(s, "default");
    return g_string_free(s, FALSE);
}

/*
 * Build the view: network session (ephemeral for "default", persistent
 * otherwise, bound to the uplink when one is given), settings, surface, and
 * the protocol proxy via create_view. The verbatim profile id is carried
 * into create_view; the on-disk path uses a sanitised directory name.
 * Returns NULL with *why set on failure. Does NOT load any URL (the caller
 * warms the default view only).
 */
static KWView *view_create(const char *profile, const char *uplink, gboolean is_default,
    char **why)
{
    *why = NULL;

    /* Duplicate detection uses the verbatim id. */
    if (g_hash_table_lookup(g_views, profile ? profile : "default")) {
        *why = g_strdup_printf("a view for profile '%s' already exists",
            profile ? profile : "default");
        return NULL;
    }

    KWView *v = g_new0(KWView, 1);
    v->profile = g_strdup(profile ? profile : "default");
    v->dir = profile_to_dir(v->profile);
    v->is_default = is_default;
    v->ephemeral = (g_strcmp0(v->profile, "default") == 0);
    v->active = TRUE;
    v->filter_hashes = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    v->cancellable = g_cancellable_new();

    char *store_dir = NULL;
    char *icon_db_desc = NULL;   /* for logging where the icon DB lives */

    if (v->ephemeral) {
        /* Profile "default": fresh ephemeral session, nothing persistent on
         * disk. Give the content filter store a throwaway temp directory.
         * The favicon database of an ephemeral session is in-memory (the
         * engine returns a null path for it), so icons work for the view's
         * lifetime but are not persisted; that is fine for the warm default. */
        v->session = session_create(NULL, NULL, uplink, why);
        if (!v->session) {
            view_free(v);
            return NULL;
        }
        GError *e = NULL;
        v->tmp_dir = g_dir_make_tmp(KW_DEFAULT_TMP_PREFIX "XXXXXX", &e);
        if (v->tmp_dir)
            store_dir = g_build_filename(v->tmp_dir, "filter-store", NULL);
        g_clear_error(&e);
        icon_db_desc = g_strdup("in-memory (ephemeral session)");
    } else {
        /* Persistent profile. Data honours XDG_DATA_HOME (g_get_user_data_dir)
         * and cache honours XDG_CACHE_HOME (g_get_user_cache_dir); GLib uses
         * the environment variables when set, which is what Kodi relies on for
         * persistence across restarts. */
        char *data_dir = g_build_filename(g_get_user_data_dir(), "kodi-webview", v->dir, NULL);
        char *cache_dir = g_build_filename(g_get_user_cache_dir(), "kodi-webview", v->dir, NULL);
        store_dir = g_build_filename(data_dir, "filter-store", NULL);
        char *cookies = g_build_filename(data_dir, "cookies.db", NULL);

        gboolean dirs_ok =
            g_mkdir_with_parents(data_dir, 0700) == 0 &&
            g_mkdir_with_parents(cache_dir, 0700) == 0 &&
            g_mkdir_with_parents(store_dir, 0700) == 0;
        if (!dirs_ok) {
            *why = g_strdup_printf("cannot create profile storage under %s: %s",
                data_dir, g_strerror(errno));
            g_free(data_dir); g_free(cache_dir); g_free(store_dir); g_free(cookies);
            view_free(v);
            return NULL;
        }

        v->session = session_create(data_dir, cache_dir, uplink, why);
        if (!v->session) {
            g_free(data_dir); g_free(cache_dir); g_free(store_dir); g_free(cookies);
            view_free(v);
            return NULL;
        }
        WebKitCookieManager *cm = webkit_network_session_get_cookie_manager(v->session);
        webkit_cookie_manager_set_persistent_storage(cm, cookies,
            WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);

        /* The engine derives the favicon DB path from the session CACHE
         * directory: <cache>/icondatabase/WebpageIcons.db. */
        icon_db_desc = g_build_filename(cache_dir, "icondatabase", NULL);

        g_free(data_dir); g_free(cache_dir); g_free(cookies);
    }

    /* Enable website icons on this view's session (off by default) so the
     * favicon database populates; get its handle for later fetches. */
    WebKitWebsiteDataManager *wdm = webkit_network_session_get_website_data_manager(v->session);
    webkit_website_data_manager_set_favicons_enabled(wdm, TRUE);
    v->favicon_db = webkit_website_data_manager_get_favicon_database(wdm);
    g_message("favicon db: %s enabled=%d dir=%s", v->profile,
        v->favicon_db ? 1 : 0, icon_db_desc ? icon_db_desc : "(unknown)");
    g_free(icon_db_desc);

    if (store_dir) {
        v->filter_store = webkit_user_content_filter_store_new(store_dir);
        g_free(store_dir);
    }
    v->ucm = webkit_user_content_manager_new();

    /* No user agent is set: the view uses the engine default until a valid
     * set_user_agent event arrives (per the protocol). */
    v->settings = webkit_settings_new_with_settings(
        "enable-spatial-navigation", TRUE,
        "enable-mediasource", TRUE,
        "media-playback-requires-user-gesture", FALSE,
        "enable-encrypted-media", TRUE,
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
    v = view_create(profile, uplink, FALSE, &why);
    if (!v) {
        kodi_web_manager_v1_view_unavailable(manager, profile,
            why ? why : "view could not be created");
        kw_flush();
    }
    g_free(why);
    /* No URL is loaded: configuration events then load_url will arrive. */
}

static const struct kodi_web_manager_v1_listener manager_listener = {
    manager_want_view
};

/* ------------------------------------------------------------------ */
/* Registry (our own, on WPE's wl_display)                             */
/* ------------------------------------------------------------------ */

static void registry_global(void *data, struct wl_registry *registry,
    uint32_t name, const char *interface, uint32_t version)
{
    (void)data;
    if (g_strcmp0(interface, kodi_web_manager_v1_interface.name) == 0 && !g_manager) {
        uint32_t bind_ver = version < 1 ? version : 1;
        g_manager = wl_registry_bind(registry, name, &kodi_web_manager_v1_interface, bind_ver);
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
    /* This path ends the process directly (Kodi stop/restart ends the client
     * here), so remove the ephemeral default views' throwaway stores before
     * exiting; _exit() runs no atexit handlers. */
    cleanup_temp_dirs();
    _exit(1);
}

static void activate(GApplication *application, gpointer user_data)
{
    WPEDisplayWayland *wayland;
    GError *error = NULL;
    char *why = NULL;
    KWView *def;

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
    g_registry = wl_display_get_registry(g_wl_display);
    wl_registry_add_listener(g_registry, &registry_listener, NULL);
    wl_display_roundtrip(g_wl_display);   /* receive and bind the manager */

    if (!g_manager) {
        g_printerr("kodi-webview: compositor does not offer kodi_web_manager_v1\n");
        g_application_quit(application);
        return;
    }

    g_views = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, view_free);

    /* 3. Warm "default" view: create_view + about:blank; ready follows once
     * the default view's first load finishes (engine processes up). */
    def = view_create("default", NULL, TRUE, &why);
    if (!def) {
        g_printerr("kodi-webview: cannot create default view: %s\n",
            why ? why : "unknown");
        g_free(why);
        g_application_quit(application);
        return;
    }
    webkit_web_view_load_uri(def->web_view, "about:blank");
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

int main(int argc, char *argv[])
{
    (void)argc;
    (void)argv;

    /* Decoder-rank policy must be in place before the engine's media
     * pipeline (and the child processes that run it) start. */
    kw_apply_codec_policy();
    kw_route_audio_to_kodi();

    /* Reap throwaway default stores left by a previous run before we start. */
    reap_stale_default_stores();

    g_app = g_application_new("org.kodi.webview", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(g_app, "activate", G_CALLBACK(activate), NULL);

    g_unix_signal_add(SIGINT, quit_on_signal, g_app);
    g_unix_signal_add(SIGTERM, quit_on_signal, g_app);

    int status = g_application_run(g_app, 0, NULL);

    if (g_views)
        g_hash_table_destroy(g_views);
    g_object_unref(g_app);
    return status;
}
