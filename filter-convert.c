/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Daniel Golle <daniel@makrotopia.org>
 *
 * Adblock Plus filter syntax to WebKit content blocker rules.
 */

#include <string.h>

#include "filter-convert.h"

#define HOST_ANCHOR "^[^:]+://+([^:/]+\\.)?"
#define SEPARATOR "[^-.%a-zA-Z0-9_]"
#define SEPARATOR_OR_END "([^-.%a-zA-Z0-9_].*)?$"
#define REGEX_SPECIAL ".+?()[]{}\\$|"

static const char *const default_types[] = {
    "child-document", "image", "style-sheet", "script", "font", "media",
    "fetch", "websocket", "ping", "other", "svg-document", NULL
};

static const struct {
    const char *option;
    const char *type;
} type_options[] = {
    { "script", "script" },
    { "image", "image" },
    { "stylesheet", "style-sheet" },
    { "css", "style-sheet" },
    { "font", "font" },
    { "media", "media" },
    { "object", "other" },
    { "xmlhttprequest", "fetch" },
    { "xhr", "fetch" },
    { "subdocument", "child-document" },
    { "frame", "child-document" },
    { "websocket", "websocket" },
    { "ping", "ping" },
    { "beacon", "ping" },
    { "other", "other" },
    { "popup", "popup" },
};

#define PROCEDURAL_PATTERN \
    ":(has-text|matches-css|matches-css-before|matches-css-after|" \
    "matches-path|matches-attr|matches-prop|matches-media|" \
    "min-text-length|upward|xpath|remove|remove-attr|remove-class|style|" \
    "watch-attr|others|contains|if|if-not|nth-ancestor|-abp-[a-z-]+)\\("

#define COSMETIC_PATTERN "^([a-zA-Z0-9.,~*_-]*)(#@?[?$%]?[?]?#)(.*)$"

typedef struct {
    GPtrArray *include;
    GPtrArray *exclude;
    guint dropped;
} DomainLists;

typedef struct {
    gboolean case_sensitive;
    gboolean important;
    gboolean document;
    gboolean elemhide;
    gboolean generichide;
    const char *load_type;
    DomainLists *domains;
    GPtrArray *types;
    GPtrArray *excluded_types;
} FilterOptions;

typedef struct {
    char *selector;
    DomainLists *lists;
} SpecificEntry;

typedef struct {
    GPtrArray *block;
    GPtrArray *important;
    GPtrArray *exception;
    GPtrArray *document_exception;
    GPtrArray *elemhide;
    GPtrArray *generichide;
    GPtrArray *generic;
    GPtrArray *specific;
    GHashTable *cosmetic_exceptions;
    GHashTable *badfilters;
    GRegex *cosmetic_re;
    GRegex *procedural_re;
    FilterStats *stats;
} ConvertState;

typedef struct {
    const char *url_filter;
    const FilterOptions *opts;
    GPtrArray *types;
    const char *domain_key;
    GPtrArray *domains;
    const char *top_url;
    const char *action;
    const char *selector;
} RuleSpec;

static void domain_lists_free(DomainLists *lists)
{
    if (!lists)
        return;
    g_ptr_array_unref(lists->include);
    g_ptr_array_unref(lists->exclude);
    g_free(lists);
}

static void specific_entry_free(gpointer data)
{
    SpecificEntry *entry = data;

    g_free(entry->selector);
    domain_lists_free(entry->lists);
    g_free(entry);
}

static void excluded_free(gpointer data)
{
    if (data)
        g_ptr_array_unref(data);
}

static void options_free(FilterOptions *opts)
{
    if (!opts)
        return;
    domain_lists_free(opts->domains);
    if (opts->types)
        g_ptr_array_unref(opts->types);
    if (opts->excluded_types)
        g_ptr_array_unref(opts->excluded_types);
    g_free(opts);
}

static void json_append_string(GString *out, const char *s)
{
    const unsigned char *p;

    g_string_append_c(out, '"');
    for (p = (const unsigned char *)s; *p; p++) {
        if (*p == '"' || *p == '\\')
            g_string_append_c(out, '\\');
        if (*p < 0x20)
            g_string_append_printf(out, "\\u%04x", *p);
        else
            g_string_append_c(out, *p);
    }
    g_string_append_c(out, '"');
}

static void json_append_array(GString *out, GPtrArray *items)
{
    guint i;

    g_string_append_c(out, '[');
    for (i = 0; i < items->len; i++) {
        if (i)
            g_string_append_c(out, ',');
        json_append_string(out, g_ptr_array_index(items, i));
    }
    g_string_append_c(out, ']');
}

static void str_array_add_uniq(GPtrArray *array, const char *s)
{
    guint i;

    for (i = 0; i < array->len; i++)
        if (!strcmp(g_ptr_array_index(array, i), s))
            return;
    g_ptr_array_add(array, (gpointer)s);
}

static gboolean str_array_has(GPtrArray *array, const char *s)
{
    guint i;

    if (!array)
        return FALSE;
    for (i = 0; i < array->len; i++)
        if (!strcmp(g_ptr_array_index(array, i), s))
            return TRUE;
    return FALSE;
}

static void pattern_char_append(GString *re, unsigned char c, gboolean last)
{
    if (c == '*')
        g_string_append(re, ".*");
    else if (c == '^' && last)
        g_string_append(re, SEPARATOR_OR_END);
    else if (c == '^')
        g_string_append(re, SEPARATOR);
    else if (strchr(REGEX_SPECIAL, c))
        g_string_append_printf(re, "\\%c", c);
    else
        g_string_append_c(re, c);
}

static char *pattern_regex(const char *pattern)
{
    gsize n = strlen(pattern);
    gboolean end = FALSE;
    GString *re;
    gsize i = 0;

    re = g_string_new(NULL);
    if (g_str_has_prefix(pattern, "||")) {
        g_string_append(re, HOST_ANCHOR);
        i = 2;
    } else if (pattern[0] == '|') {
        g_string_append_c(re, '^');
        i = 1;
    }

    if (n > i && pattern[n - 1] == '|') {
        end = TRUE;
        n--;
    }

    for (; i < n; i++) {
        if ((unsigned char)pattern[i] > 127) {
            g_string_free(re, TRUE);
            return NULL;
        }
        pattern_char_append(re, pattern[i], i == n - 1 && !end);
    }

    if (end)
        g_string_append_c(re, '$');
    if (!re->len)
        g_string_append(re, ".*");

    return g_string_free(re, FALSE);
}

static char *domain_entry(const char *start, gsize len)
{
    char *domain;
    char *entry;
    char *lower;
    char *p;

    domain = g_strstrip(g_strndup(start, len));
    lower = g_ascii_strdown(domain, -1);
    g_free(domain);

    for (p = lower; *p; p++) {
        if (!g_ascii_isdigit(*p) && !(*p >= 'a' && *p <= 'z') &&
            *p != '.' && *p != '-')
            break;
    }
    if (!*lower || *p) {
        g_free(lower);
        return NULL;
    }

    entry = g_strconcat("*", lower, NULL);
    g_free(lower);
    return entry;
}

static DomainLists *domain_lists(const char *spec, char sep)
{
    DomainLists *lists = g_new0(DomainLists, 1);
    const char *start = spec;
    gboolean negated;
    const char *end;
    char *entry;

    lists->include = g_ptr_array_new_with_free_func(g_free);
    lists->exclude = g_ptr_array_new_with_free_func(g_free);

    for (;;) {
        end = strchr(start, sep);
        if (!end)
            end = start + strlen(start);

        negated = (*start == '~');
        entry = domain_entry(start + negated, end - start - negated);
        if (!entry)
            lists->dropped++;
        else
            g_ptr_array_add(negated ? lists->exclude : lists->include, entry);

        if (!*end)
            break;
        start = end + 1;
    }

    return lists;
}

static gboolean option_types(FilterOptions *opts, const char *name, gboolean negated)
{
    GPtrArray **target = negated ? &opts->excluded_types : &opts->types;
    gboolean found = FALSE;
    guint i;

    for (i = 0; i < G_N_ELEMENTS(type_options); i++) {
        if (strcmp(type_options[i].option, name))
            continue;
        if (!*target)
            *target = g_ptr_array_new();
        g_ptr_array_add(*target, (gpointer)type_options[i].type);
        found = TRUE;
    }

    return found;
}

static void option_domains(FilterOptions *opts, const char *value, gsize len)
{
    char *spec = g_strndup(value, len);

    domain_lists_free(opts->domains);
    opts->domains = domain_lists(spec, '|');
    g_free(spec);
}

static gboolean option_flag(FilterOptions *opts, const char *name)
{
    if (!strcmp(name, "match-case"))
        opts->case_sensitive = TRUE;
    else if (!strcmp(name, "important"))
        opts->important = TRUE;
    else if (!strcmp(name, "document") || !strcmp(name, "doc"))
        opts->document = TRUE;
    else if (!strcmp(name, "elemhide") || !strcmp(name, "ehide"))
        opts->elemhide = TRUE;
    else if (!strcmp(name, "generichide") || !strcmp(name, "ghide"))
        opts->generichide = TRUE;
    else
        return FALSE;
    return TRUE;
}

static gboolean option_apply(FilterOptions *opts, const char *opt, gsize len)
{
    gboolean negated = (opt[0] == '~');
    const char *value = NULL;
    gboolean ok = TRUE;
    char *name;
    char *eq;

    name = g_ascii_strdown(opt + negated, len - negated);
    eq = strchr(name, '=');
    if (eq) {
        value = opt + negated + (eq - name) + 1;
        *eq = '\0';
    }

    if (!strcmp(name, "third-party") || !strcmp(name, "3p"))
        opts->load_type = negated ? "first-party" : "third-party";
    else if (!strcmp(name, "first-party") || !strcmp(name, "1p"))
        opts->load_type = negated ? "third-party" : "first-party";
    else if ((!strcmp(name, "domain") || !strcmp(name, "from")) && value)
        option_domains(opts, value, opt + len - value);
    else if (!option_flag(opts, name))
        ok = option_types(opts, name, negated);

    g_free(name);
    return ok;
}

static FilterOptions *options_parse(const char *spec)
{
    FilterOptions *opts = g_new0(FilterOptions, 1);
    const char *start = spec;
    const char *end;

    for (;;) {
        end = strchr(start, ',');
        if (!end)
            end = start + strlen(start);

        if (!option_apply(opts, start, end - start)) {
            options_free(opts);
            return NULL;
        }

        if (!*end)
            break;
        start = end + 1;
    }

    return opts;
}

static GPtrArray *trigger_types(const FilterOptions *opts)
{
    GPtrArray *types = g_ptr_array_new();
    const char *type;
    guint i;

    if (opts->types) {
        for (i = 0; i < opts->types->len; i++) {
            type = g_ptr_array_index(opts->types, i);
            if (!str_array_has(opts->excluded_types, type))
                str_array_add_uniq(types, type);
        }
        return types;
    }

    for (i = 0; default_types[i]; i++)
        if (!str_array_has(opts->excluded_types, default_types[i]))
            str_array_add_uniq(types, default_types[i]);
    return types;
}

static char *rule_json(const RuleSpec *spec)
{
    GString *out = g_string_new("{\"trigger\":{\"url-filter\":");

    json_append_string(out, spec->url_filter);
    if (spec->opts && spec->opts->case_sensitive)
        g_string_append(out, ",\"url-filter-is-case-sensitive\":true");
    if (spec->opts && spec->opts->load_type) {
        g_string_append(out, ",\"load-type\":[");
        json_append_string(out, spec->opts->load_type);
        g_string_append_c(out, ']');
    }
    if (spec->types) {
        g_string_append(out, ",\"resource-type\":");
        json_append_array(out, spec->types);
    }
    if (spec->domain_key) {
        g_string_append_printf(out, ",\"%s\":", spec->domain_key);
        json_append_array(out, spec->domains);
    }
    if (spec->top_url) {
        g_string_append(out, ",\"if-top-url\":[");
        json_append_string(out, spec->top_url);
        g_string_append_c(out, ']');
    }
    g_string_append(out, "},\"action\":{\"type\":");
    json_append_string(out, spec->action);
    if (spec->selector) {
        g_string_append(out, ",\"selector\":");
        json_append_string(out, spec->selector);
    }
    g_string_append(out, "}}");

    return g_string_free(out, FALSE);
}

/* A trigger takes one domain condition: excluded domains get their own rule. */
static void network_rules(GPtrArray *out, const char *re, const FilterOptions *opts,
                          const char *action)
{
    RuleSpec spec = { re, opts, NULL, NULL, NULL, NULL, action, NULL };
    DomainLists *domains = opts->domains;

    spec.types = trigger_types(opts);
    if (!spec.types->len)
        goto out;

    if (domains && domains->include->len) {
        spec.domain_key = "if-domain";
        spec.domains = domains->include;
    } else if (domains && domains->exclude->len) {
        spec.domain_key = "unless-domain";
        spec.domains = domains->exclude;
    } else if (domains) {
        goto out;
    }

    g_ptr_array_add(out, rule_json(&spec));

    if (strcmp(action, "block") || !domains || !domains->include->len ||
        !domains->exclude->len)
        goto out;

    spec.domain_key = "if-domain";
    spec.domains = domains->exclude;
    spec.action = "ignore-previous-rules";
    g_ptr_array_add(out, rule_json(&spec));

out:
    g_ptr_array_unref(spec.types);
}

static gboolean badfilter_suffix(const char *line, gsize n)
{
    return n > 9 && g_str_has_suffix(line, "badfilter") &&
           (line[n - 10] == '$' || line[n - 10] == ',');
}

static gboolean badfilter_marked(const char *line)
{
    return strstr(line, ",badfilter,") || badfilter_suffix(line, strlen(line));
}

static char *badfilter_key(const char *line)
{
    GString *key = g_string_new(line);
    char *mid;

    if (badfilter_suffix(key->str, key->len))
        g_string_truncate(key, key->len - 10);

    mid = strstr(key->str, ",badfilter,");
    if (mid)
        g_string_erase(key, mid - key->str, strlen(",badfilter"));

    return g_string_free(key, FALSE);
}

static gboolean pattern_is_regex(const char *pattern)
{
    gsize n = strlen(pattern);

    return n >= 2 && pattern[0] == '/' && pattern[n - 1] == '/';
}

static void network_exception(ConvertState *state, const char *re, FilterOptions *opts)
{
    RuleSpec spec = { ".*", NULL, NULL, NULL, NULL, NULL, "ignore-previous-rules", NULL };
    GPtrArray *types;

    if (opts->document) {
        spec.top_url = re;
        g_ptr_array_add(state->document_exception, rule_json(&spec));
        state->stats->network_exception++;
        return;
    }

    if (opts->elemhide || opts->generichide) {
        types = g_ptr_array_new();
        g_ptr_array_add(types, "top-document");
        g_ptr_array_add(types, "child-document");
        spec.url_filter = re;
        spec.opts = opts;
        spec.types = types;
        g_ptr_array_add(opts->elemhide ? state->elemhide : state->generichide,
                        rule_json(&spec));
        g_ptr_array_unref(types);
        state->stats->cosmetic_exception++;
        return;
    }

    network_rules(state->exception, re, opts, "ignore-previous-rules");
    state->stats->network_exception++;
}

static void network_block(ConvertState *state, const char *re, FilterOptions *opts)
{
    if (opts->elemhide || opts->generichide) {
        state->stats->unsupported_option++;
        return;
    }

    if (opts->document) {
        if (!opts->types)
            opts->types = g_ptr_array_new();
        g_ptr_array_add(opts->types, "top-document");
    }

    network_rules(opts->important ? state->important : state->block, re, opts, "block");
    state->stats->network++;
}

static void network_parse(ConvertState *state, const char *line)
{
    FilterOptions *opts;
    gboolean exception;
    const char *dollar;
    const char *body;
    char *pattern;
    char *re;

    exception = g_str_has_prefix(line, "@@");
    body = exception ? line + 2 : line;

    dollar = strrchr(body, '$');
    if (dollar && strcmp(dollar, "$/")) {
        opts = options_parse(dollar + 1);
        pattern = g_strndup(body, dollar - body);
    } else {
        opts = g_new0(FilterOptions, 1);
        pattern = g_strdup(body);
    }

    if (!opts) {
        state->stats->unsupported_option++;
        goto out;
    }

    if (opts->domains && opts->domains->dropped && !opts->domains->include->len &&
        !opts->domains->exclude->len) {
        state->stats->unsupported_domain++;
        goto out;
    }

    if (pattern_is_regex(pattern)) {
        state->stats->regex++;
        goto out;
    }

    re = pattern_regex(pattern);
    if (!re) {
        state->stats->non_ascii++;
        goto out;
    }

    if (exception)
        network_exception(state, re, opts);
    else
        network_block(state, re, opts);
    g_free(re);

out:
    options_free(opts);
    g_free(pattern);
}

static gboolean selector_is_ascii(const char *selector)
{
    const unsigned char *p;

    for (p = (const unsigned char *)selector; *p; p++)
        if (*p > 0x7f)
            return FALSE;
    return TRUE;
}

static void cosmetic_exception_add(ConvertState *state, const char *domains,
                                   const char *selector, DomainLists *lists)
{
    GPtrArray *excluded;
    gpointer value;
    guint i;

    if (!*domains) {
        g_hash_table_replace(state->cosmetic_exceptions, g_strdup(selector), NULL);
        return;
    }

    if (!g_hash_table_lookup_extended(state->cosmetic_exceptions, selector, NULL, &value)) {
        excluded = g_ptr_array_new_with_free_func(g_free);
        g_hash_table_insert(state->cosmetic_exceptions, g_strdup(selector), excluded);
    } else if (!value) {
        return;
    } else {
        excluded = value;
    }

    for (i = 0; i < lists->include->len; i++)
        g_ptr_array_add(excluded, g_strdup(g_ptr_array_index(lists->include, i)));
}

static void cosmetic_specific_add(ConvertState *state, const char *selector,
                                  DomainLists *lists)
{
    SpecificEntry *entry = g_new0(SpecificEntry, 1);

    entry->selector = g_strdup(selector);
    entry->lists = lists;
    g_ptr_array_add(state->specific, entry);
}

static void cosmetic_parse(ConvertState *state, const char *domains, const char *separator,
                           const char *selector)
{
    DomainLists *lists;

    if (strcmp(separator, "##") && strcmp(separator, "#@#")) {
        state->stats->extended_cosmetic++;
        return;
    }

    if (g_str_has_prefix(selector, "+js") || selector[0] == '^' ||
        g_regex_match(state->procedural_re, selector, 0, NULL)) {
        state->stats->extended_cosmetic++;
        return;
    }

    if (!selector_is_ascii(selector)) {
        state->stats->non_ascii++;
        return;
    }

    lists = domain_lists(domains, ',');

    if (!strcmp(separator, "#@#")) {
        cosmetic_exception_add(state, domains, selector, lists);
        state->stats->cosmetic_exception++;
    } else if (!*domains) {
        g_ptr_array_add(state->generic, g_strdup(selector));
        state->stats->cosmetic++;
    } else if (!lists->include->len && !lists->exclude->len) {
        state->stats->unsupported_domain++;
    } else {
        cosmetic_specific_add(state, selector, lists);
        state->stats->cosmetic++;
        return;
    }

    domain_lists_free(lists);
}

static char *cosmetic_generic_rule(ConvertState *state, const char *selector)
{
    RuleSpec spec = { ".*", NULL, NULL, NULL, NULL, NULL, "css-display-none", selector };
    GPtrArray *excluded = NULL;
    GPtrArray *uniq;
    gpointer value;
    char *json;
    guint i;

    if (g_hash_table_lookup_extended(state->cosmetic_exceptions, selector, NULL, &value)) {
        if (!value)
            return NULL;
        excluded = value;
    }

    if (!excluded || !excluded->len)
        return rule_json(&spec);

    uniq = g_ptr_array_new();
    for (i = 0; i < excluded->len; i++)
        str_array_add_uniq(uniq, g_ptr_array_index(excluded, i));
    spec.domain_key = "unless-domain";
    spec.domains = uniq;
    json = rule_json(&spec);
    g_ptr_array_unref(uniq);
    return json;
}

static char *cosmetic_specific_rule(ConvertState *state, SpecificEntry *entry)
{
    RuleSpec spec = { ".*", NULL, NULL, NULL, NULL, NULL, "css-display-none", NULL };
    GPtrArray *excluded = NULL;
    GPtrArray *include;
    char *json = NULL;
    gpointer value;
    const char *d;
    guint i;

    spec.selector = entry->selector;
    if (g_hash_table_lookup_extended(state->cosmetic_exceptions, entry->selector, NULL,
                                     &value)) {
        if (!value)
            return NULL;
        excluded = value;
    }

    include = g_ptr_array_new();
    for (i = 0; i < entry->lists->include->len; i++) {
        d = g_ptr_array_index(entry->lists->include, i);
        if (!str_array_has(excluded, d))
            g_ptr_array_add(include, (gpointer)d);
    }

    if (include->len) {
        spec.domain_key = "if-domain";
        spec.domains = include;
        json = rule_json(&spec);
    } else if (!entry->lists->include->len) {
        spec.domain_key = "unless-domain";
        spec.domains = entry->lists->exclude;
        json = rule_json(&spec);
    }

    g_ptr_array_unref(include);
    return json;
}

static void cosmetic_match(ConvertState *state, GMatchInfo *match)
{
    char *separator;
    char *selector;
    char *domains;

    domains = g_match_info_fetch(match, 1);
    separator = g_match_info_fetch(match, 2);
    selector = g_match_info_fetch(match, 3);
    cosmetic_parse(state, domains, separator, selector);
    g_free(domains);
    g_free(separator);
    g_free(selector);
}

static void line_classify(ConvertState *state, const char *line)
{
    GMatchInfo *match = NULL;
    gboolean cosmetic;
    gboolean bad;
    char *key;

    if (!*line || line[0] == '!' || line[0] == '[')
        return;

    cosmetic = g_regex_match(state->cosmetic_re, line, 0, &match);
    if (cosmetic)
        cosmetic_match(state, match);
    g_match_info_free(match);
    if (cosmetic)
        return;

    if (strstr(line, "$$") || strstr(line, "$@$")) {
        state->stats->extended_cosmetic++;
        return;
    }

    key = badfilter_key(line);
    bad = g_hash_table_contains(state->badfilters, key);
    g_free(key);
    if (bad) {
        state->stats->badfilter++;
        return;
    }

    network_parse(state, line);
}

static void rules_append(GPtrArray *rules, GPtrArray *items)
{
    guint i;

    for (i = 0; i < items->len; i++)
        g_ptr_array_add(rules, g_ptr_array_index(items, i));
}

static void state_init(ConvertState *state, FilterStats *stats)
{
    memset(state, 0, sizeof(*state));
    memset(stats, 0, sizeof(*stats));
    state->stats = stats;
    state->block = g_ptr_array_new_with_free_func(g_free);
    state->important = g_ptr_array_new_with_free_func(g_free);
    state->exception = g_ptr_array_new_with_free_func(g_free);
    state->document_exception = g_ptr_array_new_with_free_func(g_free);
    state->elemhide = g_ptr_array_new_with_free_func(g_free);
    state->generichide = g_ptr_array_new_with_free_func(g_free);
    state->generic = g_ptr_array_new_with_free_func(g_free);
    state->specific = g_ptr_array_new_with_free_func(specific_entry_free);
    state->cosmetic_exceptions = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
                                                       excluded_free);
    state->badfilters = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    state->cosmetic_re = g_regex_new(COSMETIC_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
    state->procedural_re = g_regex_new(PROCEDURAL_PATTERN, G_REGEX_OPTIMIZE, 0, NULL);
}

static void state_clear(ConvertState *state)
{
    g_ptr_array_unref(state->block);
    g_ptr_array_unref(state->important);
    g_ptr_array_unref(state->exception);
    g_ptr_array_unref(state->document_exception);
    g_ptr_array_unref(state->elemhide);
    g_ptr_array_unref(state->generichide);
    g_ptr_array_unref(state->generic);
    g_ptr_array_unref(state->specific);
    g_hash_table_destroy(state->cosmetic_exceptions);
    g_hash_table_destroy(state->badfilters);
    g_regex_unref(state->cosmetic_re);
    g_regex_unref(state->procedural_re);
}

static void cosmetic_rules(ConvertState *state, GPtrArray *generic, GPtrArray *specific)
{
    char *rule;
    guint i;

    for (i = 0; i < state->generic->len; i++) {
        rule = cosmetic_generic_rule(state, g_ptr_array_index(state->generic, i));
        if (rule)
            g_ptr_array_add(generic, rule);
    }

    for (i = 0; i < state->specific->len; i++) {
        rule = cosmetic_specific_rule(state, g_ptr_array_index(state->specific, i));
        if (rule)
            g_ptr_array_add(specific, rule);
    }
}

/* ignore-previous-rules cancels only earlier rules: this order is the ABP precedence */
static GString *rules_serialise(ConvertState *state, guint *rule_count)
{
    GPtrArray *specific = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *generic = g_ptr_array_new_with_free_func(g_free);
    GPtrArray *rules = g_ptr_array_new();
    GString *json;
    guint i;

    cosmetic_rules(state, generic, specific);

    rules_append(rules, generic);
    rules_append(rules, state->generichide);
    rules_append(rules, specific);
    rules_append(rules, state->elemhide);
    rules_append(rules, state->block);
    rules_append(rules, state->exception);
    rules_append(rules, state->important);
    rules_append(rules, state->document_exception);

    json = g_string_new("[");
    for (i = 0; i < rules->len; i++) {
        g_string_append(json, i ? ",\n" : "\n");
        g_string_append(json, g_ptr_array_index(rules, i));
    }
    g_string_append(json, "\n]\n");

    *rule_count = rules->len;
    g_ptr_array_unref(rules);
    g_ptr_array_unref(generic);
    g_ptr_array_unref(specific);
    return json;
}

GString *filter_convert(const char *text, gsize len, guint *rule_count, FilterStats *stats)
{
    ConvertState state;
    GString *json;
    char **lines;
    char *copy;
    guint i;

    state_init(&state, stats);

    copy = g_strndup(text, len);
    lines = g_strsplit(copy, "\n", -1);
    g_free(copy);
    for (i = 0; lines[i]; i++)
        g_strstrip(lines[i]);

    for (i = 0; lines[i]; i++)
        if (badfilter_marked(lines[i]))
            g_hash_table_add(state.badfilters, badfilter_key(lines[i]));

    for (i = 0; lines[i]; i++)
        line_classify(&state, lines[i]);

    json = rules_serialise(&state, rule_count);

    g_strfreev(lines);
    state_clear(&state);
    return json;
}

char *filter_stats_format(const FilterStats *stats)
{
    return g_strdup_printf("network %u, network exceptions %u, cosmetic %u, "
                           "cosmetic exceptions %u; skipped: extended cosmetic %u, "
                           "regex %u, unsupported option %u, unsupported domain %u, "
                           "non-ASCII %u, badfilter %u",
                           stats->network, stats->network_exception, stats->cosmetic,
                           stats->cosmetic_exception, stats->extended_cosmetic,
                           stats->regex, stats->unsupported_option,
                           stats->unsupported_domain, stats->non_ascii, stats->badfilter);
}
