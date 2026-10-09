/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Settings registry, see cfg_registry.h. Pure: no Zephyr, no ZMK.
 */

#include "cfg_registry.h"

#include <errno.h>
#include <string.h>

static const struct cfg_def *defs;
static uint8_t n_defs;
static uint32_t *vals;
static struct cfg_list_slot *lists;
static uint32_t rev;
static uint32_t dirty;

uint8_t cfg_count(void) { return n_defs; }

const struct cfg_def *cfg_def(uint8_t i) { return i < n_defs ? &defs[i] : NULL; }

static bool same(const char *a, const char *b, size_t len) {
    return strlen(a) == len && memcmp(a, b, len) == 0;
}

int cfg_find(const char *key, size_t len) {
    for (uint8_t i = 0; i < n_defs; i++) {
        if (same(defs[i].key, key, len)) {
            return i;
        }
    }
    return -ENOENT;
}

uint8_t cfg_name_count(const struct cfg_def *d) {
    uint8_t n = 0;

    if (d->names == NULL) {
        return 0;
    }
    while (n < UINT8_MAX && d->names(n) != NULL) {
        n++;
    }
    return n;
}

int cfg_name_find(const struct cfg_def *d, const char *s, size_t len) {
    uint8_t n = cfg_name_count(d);

    for (uint8_t j = 0; j < n; j++) {
        if (same(d->names(j), s, len)) {
            return j;
        }
    }
    return -EINVAL;
}

static void list_default(const struct cfg_def *d, struct cfg_value *v) {
    uint8_t n = cfg_name_count(d);

    v->len = n < CFG_LIST_MAX ? n : CFG_LIST_MAX;
    for (uint8_t j = 0; j < v->len; j++) {
        v->idx[j] = j;
    }
}

static void default_value(const struct cfg_def *d, struct cfg_value *v) {
    memset(v, 0, sizeof(*v));
    if (d->type == CFG_LIST) {
        list_default(d, v);
    } else {
        v->u = d->def;
    }
}

/* Write a checked value to its place: list slot, owner or vals[]. */
static void put(uint8_t i, const struct cfg_value *v) {
    const struct cfg_def *d = &defs[i];

    if (d->type == CFG_LIST) {
        struct cfg_list_slot *s = &lists[vals[i]];

        s->len = v->len;
        memcpy(s->idx, v->idx, v->len);
    } else if (d->flags & CFG_F_PROXY) {
        d->set(d->arg, v->u);
    } else {
        vals[i] = v->u;
    }
}

int cfg_init(const struct cfg_def *d, uint8_t n, uint32_t *v, struct cfg_list_slot *l,
             uint8_t n_lists) {
    uint8_t slot = 0;

    if (n > CFG_MAX) {
        return -EINVAL;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (d[i].type != CFG_LIST) {
            continue;
        }
        if (slot >= n_lists || (d[i].flags & CFG_F_PROXY)) {
            return -EINVAL;
        }
        v[i] = slot++;
    }
    defs = d;
    n_defs = n;
    vals = v;
    lists = l;
    rev = 0;
    dirty = 0;
    for (uint8_t i = 0; i < n; i++) {
        struct cfg_value dv;

        if (d[i].flags & CFG_F_PROXY) {
            continue; /* owned and initialized by the owner */
        }
        default_value(&d[i], &dv);
        put(i, &dv);
    }
    return 0;
}

void cfg_get(uint8_t i, struct cfg_value *out) {
    const struct cfg_def *d = &defs[i];

    memset(out, 0, sizeof(*out));
    if (d->type == CFG_LIST) {
        const struct cfg_list_slot *s = &lists[vals[i]];

        out->len = s->len;
        memcpy(out->idx, s->idx, s->len);
    } else {
        out->u = (d->flags & CFG_F_PROXY) ? d->get(d->arg) : vals[i];
    }
}

uint32_t cfg_u(uint8_t i) {
    struct cfg_value v;

    cfg_get(i, &v);
    return v.u;
}

/* Validate in against d; on success *out is the normalized value. */
static int check(const struct cfg_def *d, const struct cfg_value *in, struct cfg_value *out) {
    memset(out, 0, sizeof(*out));
    switch (d->type) {
    case CFG_BOOL:
        if (in->u > 1) {
            return -EINVAL;
        }
        break;
    case CFG_UINT:
        if (in->u < d->min || in->u > d->max) {
            return -EINVAL;
        }
        break;
    case CFG_ENUM:
        if (in->u >= cfg_name_count(d)) {
            return -EINVAL;
        }
        break;
    case CFG_COLOR:
        if (in->u > 0xFFFFFFu) {
            return -EINVAL;
        }
        break;
    case CFG_LIST: {
        uint8_t n = cfg_name_count(d);

        if (in->len > CFG_LIST_MAX) {
            return -EINVAL;
        }
        for (uint8_t j = 0; j < in->len; j++) {
            uint8_t x = in->idx[j];
            bool dup = false;

            if (x >= n) {
                return -EINVAL;
            }
            for (uint8_t k = 0; k < out->len; k++) {
                dup = dup || out->idx[k] == x;
            }
            if (!dup) {
                out->idx[out->len++] = x;
            }
        }
        return 0;
    }
    default:
        return -EINVAL;
    }
    out->u = in->u;
    return 0;
}

int cfg_set(uint8_t i, const struct cfg_value *in, struct cfg_value *stored) {
    struct cfg_value v;
    int rc;

    if (i >= n_defs) {
        return -ENOENT;
    }
    if (defs[i].flags & CFG_F_RO) {
        return -EACCES;
    }
    rc = check(&defs[i], in, &v);
    if (rc != 0) {
        return rc;
    }
    put(i, &v);
    rev++;
    if (!(defs[i].flags & CFG_F_PROXY)) {
        dirty |= 1u << i;
    }
    if (stored != NULL) {
        cfg_get(i, stored);
    }
    return 0;
}

int cfg_reset(uint8_t i) {
    struct cfg_value v;

    if (i >= n_defs) {
        return -ENOENT;
    }
    default_value(&defs[i], &v);
    return cfg_set(i, &v, NULL);
}

int cfg_load(uint8_t i, const struct cfg_value *in) {
    struct cfg_value v;
    int rc;

    if (i >= n_defs) {
        return -ENOENT;
    }
    if (defs[i].flags & (CFG_F_RO | CFG_F_PROXY)) {
        return -EACCES;
    }
    rc = check(&defs[i], in, &v);
    if (rc == 0) {
        put(i, &v);
    }
    return rc;
}

uint32_t cfg_rev(void) { return rev; }

void cfg_note_change(void) { rev++; }

uint32_t cfg_take_dirty(void) {
    uint32_t d = dirty;

    dirty = 0;
    return d;
}
