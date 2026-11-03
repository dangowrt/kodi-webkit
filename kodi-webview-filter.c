/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Daniel Golle <daniel@makrotopia.org>
 *
 * Compile a filter list in Adblock Plus syntax, read from stdin, into the
 * content filter store STORE, unless the store holds it already, and print
 * the identifier it is stored under.
 *
 * usage: kodi-webview-filter STORE
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <sys/prctl.h>
#include <unistd.h>

#include <wpe/webkit.h>

#include "filter-convert.h"

typedef struct {
    WebKitUserContentFilterStore *store;
    GMainLoop *loop;
    GBytes *json;
    char *id;
    int status;
} FilterJob;

static void job_saved(GObject *source, GAsyncResult *res, gpointer data)
{
    WebKitUserContentFilterStore *store = WEBKIT_USER_CONTENT_FILTER_STORE(source);
    WebKitUserContentFilter *filter;
    FilterJob *job = data;
    GError *error = NULL;

    filter = webkit_user_content_filter_store_save_finish(store, res, &error);
    if (!filter) {
        fprintf(stderr, "compiling failed: %s\n", error->message);
        g_error_free(error);
        job->status = 1;
    } else {
        webkit_user_content_filter_unref(filter);
        job->status = 0;
    }

    g_main_loop_quit(job->loop);
}

static void job_loaded(GObject *source, GAsyncResult *res, gpointer data)
{
    WebKitUserContentFilterStore *store = WEBKIT_USER_CONTENT_FILTER_STORE(source);
    WebKitUserContentFilter *filter;
    FilterJob *job = data;
    GError *error = NULL;

    filter = webkit_user_content_filter_store_load_finish(store, res, &error);
    if (filter) {
        webkit_user_content_filter_unref(filter);
        job->status = 0;
        g_main_loop_quit(job->loop);
        return;
    }

    g_error_free(error);
    webkit_user_content_filter_store_save(store, job->id, job->json, NULL, job_saved, job);
}

static GBytes *stdin_read(void)
{
    GByteArray *buf = g_byte_array_new();
    guint8 chunk[65536];
    ssize_t n;

    for (;;) {
        n = read(STDIN_FILENO, chunk, sizeof(chunk));
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        g_byte_array_append(buf, chunk, n);
    }

    if (n < 0) {
        perror("stdin");
        g_byte_array_unref(buf);
        return NULL;
    }

    return g_byte_array_free_to_bytes(buf);
}

static GBytes *list_convert(GBytes *list)
{
    FilterStats stats;
    const char *text;
    GString *json;
    gsize len = 0;
    guint count;
    char *line;

    text = g_bytes_get_data(list, &len);
    if (!g_utf8_validate_len(text, len, NULL)) {
        fprintf(stderr, "the list is not UTF-8 text\n");
        return NULL;
    }

    json = filter_convert(text, len, &count, &stats);
    line = filter_stats_format(&stats);
    fprintf(stderr, "%u rules: %s\n", count, line);
    g_free(line);

    if (!count || count > FILTER_MAX_RULES) {
        fprintf(stderr, "%u rules, WebKit takes 1 to %u\n", count, FILTER_MAX_RULES);
        g_string_free(json, TRUE);
        return NULL;
    }

    return g_string_free_to_bytes(json);
}

int main(int argc, char **argv)
{
    FilterJob job = { NULL, NULL, NULL, NULL, 1 };
    GBytes *list;

    if (argc != 2) {
        fprintf(stderr, "usage: %s STORE\n", argv[0]);
        return 2;
    }

    /* the client has no use for a compile it can no longer receive */
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || getppid() == 1)
        return 1;

    list = stdin_read();
    if (!list)
        return 1;

    job.json = list_convert(list);
    g_bytes_unref(list);
    if (!job.json)
        return 1;

    job.id = g_compute_checksum_for_bytes(G_CHECKSUM_SHA256, job.json);
    job.loop = g_main_loop_new(NULL, FALSE);
    job.store = webkit_user_content_filter_store_new(argv[1]);

    webkit_user_content_filter_store_load(job.store, job.id, NULL, job_loaded, &job);
    g_main_loop_run(job.loop);

    if (!job.status)
        printf("%s\n", job.id);

    g_object_unref(job.store);
    g_main_loop_unref(job.loop);
    g_bytes_unref(job.json);
    g_free(job.id);
    return job.status;
}
