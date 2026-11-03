/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Copyright (C) 2026 Daniel Golle <daniel@makrotopia.org>
 */

#ifndef FILTER_CONVERT_H
#define FILTER_CONVERT_H

#include <glib.h>

/* maxRuleCount in WebKit's ContentExtensionParser.cpp */
#define FILTER_MAX_RULES 150000

typedef struct {
    guint network;
    guint network_exception;
    guint cosmetic;
    guint cosmetic_exception;
    guint extended_cosmetic;
    guint regex;
    guint unsupported_option;
    guint unsupported_domain;
    guint non_ascii;
    guint badfilter;
} FilterStats;

/* Adblock Plus filter syntax to a WebKit content blocker JSON array */
GString *filter_convert(const char *text, gsize len, guint *rule_count,
                        FilterStats *stats);

char *filter_stats_format(const FilterStats *stats);

#endif
