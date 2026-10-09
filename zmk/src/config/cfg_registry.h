/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime settings registry: the decisions, pure and ZMK-free (host tested
 * in tests/). cfg_table.c holds the real table and wires the owners,
 * cfg_store.c persists it, cfg_mgmt.c serves it over mcumgr group 67 (see
 * docs/config-protocol.md for the protocol and the compatibility rules).
 *
 * Every setting has a stable key ("rgb.effect"), a type, a range or a list
 * of names, and a default. cfg_set() refuses invalid values (type, range,
 * unknown name, list too long), normalizes valid ones (duplicates in a list
 * are dropped, the first one kept) and applies them. Proxied settings live
 * in their owner (the rainy_rgb state record): the registry forwards to the
 * owner's get / set hooks, leaves them alone at init and does not mark them
 * dirty, the owner persists them. All other settings live here and are
 * marked dirty for the store. Lists cannot be proxied.
 *
 * rev changes on every change from any source: cfg_set() / cfg_reset(), and
 * owners call cfg_note_change() when a proxied value changes elsewhere (Fn
 * keys). Hosts poll it to notice changes. cfg_load() (the store's path at
 * boot) changes neither rev nor the dirty mask.
 */

#ifndef RAINY75_CFG_REGISTRY_H
#define RAINY75_CFG_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_MAX 32      /* settings in one table (the dirty mask has 32 bits) */
#define CFG_LIST_MAX 16 /* entries in one list value */

#define CFG_F_RO 0x01    /* read-only: set, reset and load are refused */
#define CFG_F_PROXY 0x02 /* lives in its owner: get / set through the hooks */

enum cfg_type {
    CFG_BOOL = 'b',
    CFG_UINT = 'u',
    CFG_ENUM = 'e',
    CFG_COLOR = 'c',
    CFG_LIST = 'l',
};

/* Name at index idx of an enum or list setting, NULL past the end. */
typedef const char *(*cfg_name_fn)(uint8_t idx);

struct cfg_def {
    const char *key;
    char type;      /* enum cfg_type */
    uint8_t flags;  /* CFG_F_* */
    uint32_t min;   /* CFG_UINT */
    uint32_t max;   /* CFG_UINT */
    uint32_t def;   /* default; CFG_LIST: unused, the default is all names in order */
    cfg_name_fn names;                   /* CFG_ENUM, CFG_LIST */
    uint32_t (*get)(uint8_t arg);        /* CFG_F_PROXY */
    void (*set)(uint8_t arg, uint32_t v); /* CFG_F_PROXY, not needed with CFG_F_RO */
    uint8_t arg;                         /* passed to get / set */
};

/* One value of any type, for passing in and out. */
struct cfg_value {
    uint32_t u;                /* bool 0/1, uint, enum name index, colour 0xRRGGBB */
    uint8_t len;               /* CFG_LIST: entries used in idx[] */
    uint8_t idx[CFG_LIST_MAX]; /* CFG_LIST: name indexes, in order */
};

/* Storage of one list setting; the table provides one slot per CFG_LIST def. */
struct cfg_list_slot {
    uint8_t len;
    uint8_t idx[CFG_LIST_MAX];
};

/* Take the table and its storage and set every non-proxied value to its
 * default. vals[n] holds the scalar values (for a list: its slot number),
 * lists[n_lists] one slot per CFG_LIST def, assigned in table order.
 * 0, or -EINVAL: more than CFG_MAX settings, more lists than slots, or a
 * proxied list. */
int cfg_init(const struct cfg_def *defs, uint8_t n, uint32_t *vals, struct cfg_list_slot *lists,
             uint8_t n_lists);

uint8_t cfg_count(void);
const struct cfg_def *cfg_def(uint8_t i); /* NULL past the end */
int cfg_find(const char *key, size_t len); /* index, or -ENOENT */
uint8_t cfg_name_count(const struct cfg_def *d);
int cfg_name_find(const struct cfg_def *d, const char *s, size_t len); /* index, or -EINVAL */

void cfg_get(uint8_t i, struct cfg_value *out);
uint32_t cfg_u(uint8_t i); /* scalar value (bool, uint, enum index, colour) for owners */

/* Validate, normalize and apply; on success bump rev, mark a non-proxied
 * setting dirty and copy the value as stored to *stored (may be NULL).
 * 0, -ENOENT (no such index), -EINVAL, -EACCES (read-only). */
int cfg_set(uint8_t i, const struct cfg_value *in, struct cfg_value *stored);
/* Back to the default, like cfg_set(): 0, -ENOENT or -EACCES. */
int cfg_reset(uint8_t i);
/* The store's path: validate and apply like cfg_set(), without rev and
 * dirty. -EACCES for proxied and read-only settings. */
int cfg_load(uint8_t i, const struct cfg_value *in);

uint32_t cfg_rev(void);
void cfg_note_change(void);    /* owners: a proxied value changed elsewhere */
uint32_t cfg_take_dirty(void); /* bit i: setting i changed since the last call */

#endif /* RAINY75_CFG_REGISTRY_H */
