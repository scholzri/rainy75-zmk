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
static uint32_t (*lock_take)(void);
static void (*lock_give)(uint32_t key);

void cfg_set_lock(uint32_t (*take)(void), void (*give)(uint32_t key)) {
    lock_take = take;
    lock_give = give;
}

static uint32_t take_lock(void) { return lock_take != NULL ? lock_take() : 0; }

static void give_lock(uint32_t key) {
    if (lock_give != NULL) {
        lock_give(key);
    }
}

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

    if (d == NULL || d->names == NULL) {
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

/* Write a checked value to its place: list slot (under the lock), owner or
 * vals[]. */
static void put(uint8_t i, const struct cfg_value *v) {
    const struct cfg_def *d = &defs[i];

    if (d->type == CFG_LIST) {
        struct cfg_list_slot *s = &lists[vals[i]];
        uint32_t key = take_lock();

        s->len = v->len;
        memcpy(s->idx, v->idx, v->len);
        give_lock(key);
    } else if (d->flags & CFG_F_PROXY) {
        d->set(d->arg, v->u);
    } else {
        vals[i] = v->u;
    }
}

/* A def that cfg_init() accepts, see cfg_registry.h. */
static bool valid_def(const struct cfg_def *d) {
    size_t kl = d->key != NULL ? strlen(d->key) : 0;
    uint8_t names = cfg_name_count(d);
    struct cfg_value dv, out;

    if (kl == 0 || kl > CFG_KEY_MAX) {
        return false;
    }
    if (d->flags & CFG_F_PROXY) {
        if (d->type == CFG_LIST || d->get == NULL) {
            return false;
        }
        if (!(d->flags & CFG_F_RO) && d->set == NULL) {
            return false;
        }
    } else if (d->flags & CFG_F_RO) {
        return false; /* nothing could ever change it */
    }
    if ((d->type == CFG_ENUM || d->type == CFG_LIST) && names == 0) {
        return false;
    }
    if (d->type == CFG_LIST && names > CFG_LIST_MAX) {
        return false;
    }
    if (d->type == CFG_UINT && d->min > d->max) {
        return false;
    }
    default_value(d, &dv);
    return check(d, &dv, &out) == 0;
}

/* After a non-proxied value changed: tell its owner. */
static void notify(uint8_t i) {
    if (!(defs[i].flags & CFG_F_PROXY) && defs[i].notify != NULL) {
        defs[i].notify(i);
    }
}

int cfg_init(const struct cfg_def *d, uint8_t n, uint32_t *v, struct cfg_list_slot *l,
             uint8_t n_lists) {
    uint8_t n_list_defs = 0, slot = 0;

    if (n > CFG_MAX) {
        return -EINVAL;
    }
    for (uint8_t i = 0; i < n; i++) {
        if (!valid_def(&d[i])) {
            return -EINVAL;
        }
        for (uint8_t j = 0; j < i; j++) {
            if (strcmp(d[i].key, d[j].key) == 0) {
                return -EINVAL;
            }
        }
        if (d[i].type == CFG_LIST) {
            n_list_defs++;
        }
    }
    if (n_list_defs > n_lists) {
        return -EINVAL;
    }
    /* valid: take it */
    defs = d;
    n_defs = n;
    vals = v;
    lists = l;
    __atomic_store_n(&rev, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&dirty, 0, __ATOMIC_RELAXED);
    for (uint8_t i = 0; i < n; i++) {
        if (d[i].type == CFG_LIST) {
            v[i] = slot++;
        }
    }
    for (uint8_t i = 0; i < n; i++) {
        struct cfg_value dv;

        if (d[i].flags & CFG_F_PROXY) {
            continue; /* owned and initialized by the owner */
        }
        default_value(&d[i], &dv);
        put(i, &dv);
    }
    for (uint8_t i = 0; i < n; i++) {
        notify(i); /* all defaults are in place before any owner reads one */
    }
    return 0;
}

void cfg_get(uint8_t i, struct cfg_value *out) {
    const struct cfg_def *d;

    memset(out, 0, sizeof(*out));
    if (i >= n_defs) {
        return;
    }
    d = &defs[i];
    if (d->type == CFG_LIST) {
        const struct cfg_list_slot *s = &lists[vals[i]];
        uint32_t key = take_lock();

        out->len = s->len;
        memcpy(out->idx, s->idx, s->len);
        give_lock(key);
    } else {
        out->u = (d->flags & CFG_F_PROXY) ? d->get(d->arg) : vals[i];
    }
}

uint32_t cfg_u(uint8_t i) {
    struct cfg_value v;

    cfg_get(i, &v);
    return v.u;
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
    __atomic_fetch_add(&rev, 1, __ATOMIC_RELAXED);
    if (!(defs[i].flags & CFG_F_PROXY)) {
        __atomic_fetch_or(&dirty, 1u << i, __ATOMIC_RELAXED);
        notify(i);
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
        notify(i);
    }
    return rc;
}

bool cfg_is_default(uint8_t i) {
    struct cfg_value cur, dv;

    if (i >= n_defs) {
        return false;
    }
    cfg_get(i, &cur);
    default_value(&defs[i], &dv);
    if (defs[i].type == CFG_LIST) {
        return cur.len == dv.len && memcmp(cur.idx, dv.idx, cur.len) == 0;
    }
    return cur.u == dv.u;
}

int cfg_cycle_next(uint8_t i, uint8_t cur) {
    struct cfg_value v;
    uint8_t n;

    if (i >= n_defs || defs[i].type != CFG_LIST) {
        return -ENOENT;
    }
    n = cfg_name_count(&defs[i]); /* > 0, cfg_init() checked it */
    cfg_get(i, &v);
    if (v.len == 0) {
        return (cur + 1) % n;
    }
    for (uint8_t j = 0; j < v.len; j++) {
        if (v.idx[j] == cur) {
            return v.idx[(j + 1) % v.len];
        }
    }
    return v.idx[0];
}

uint32_t cfg_rev(void) { return __atomic_load_n(&rev, __ATOMIC_RELAXED); }

void cfg_note_change(void) { __atomic_fetch_add(&rev, 1, __ATOMIC_RELAXED); }

uint32_t cfg_take_dirty(void) { return __atomic_exchange_n(&dirty, 0, __ATOMIC_RELAXED); }

void cfg_mark_dirty(uint32_t mask) { __atomic_fetch_or(&dirty, mask, __ATOMIC_RELAXED); }
