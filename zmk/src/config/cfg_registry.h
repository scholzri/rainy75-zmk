/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Runtime settings registry: the decisions, pure and ZMK-free (host tested
 * in tests/). cfg_table.c holds the real table and wires the owners,
 * cfg_store.c persists it (stored form in cfg_codec.c), cfg_mgmt.c serves it
 * over mcumgr group 67 (see docs/config-protocol.md for the protocol and the
 * compatibility rules).
 *
 * Every setting has a stable key ("rgb.effect"), a type, a range or a list
 * of names, and a default. cfg_set() refuses invalid values (type, range,
 * unknown name, list too long), normalizes valid ones (duplicates in a list
 * are dropped, the first one kept) and applies them. Proxied settings live
 * in their owner (the rainy_rgb state record): the registry forwards to the
 * owner's get / set hooks, leaves them alone at init and does not mark them
 * dirty, the owner persists them. All other settings live here, are marked
 * dirty for the store and reported to their owner through the def's notify
 * callback after cfg_init() and after every set, reset and load. Lists
 * cannot be proxied.
 *
 * rev changes on every change from any source: cfg_set() / cfg_reset(), and
 * owners call cfg_note_change() when a proxied value changes elsewhere (Fn
 * keys). Hosts poll it to notice changes. cfg_load() (the store's path at
 * boot) changes neither rev nor the dirty mask.
 *
 * Threads: set / reset run on the mcumgr work queue (preemptible), the Fn
 * keys and the store on the system work queue (cooperative), load in the
 * ZMK main thread, and owners read from the render thread. Scalar values
 * are single aligned 32-bit words (atomic loads and stores on this core);
 * rev and the dirty mask use atomic read-modify-write; list values are
 * copied in and out under the lock given to cfg_set_lock() (a spinlock in
 * the firmware, none in the host tests), held for the copy only.
 */

#ifndef RAINY75_CFG_REGISTRY_H
#define RAINY75_CFG_REGISTRY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CFG_MAX 32      /* settings in one table (the dirty mask has 32 bits) */
#define CFG_LIST_MAX 16 /* entries in one list value, and names of one list setting */
#define CFG_KEY_MAX 31  /* longest key: "rainy_cfg/<key>" fits the store's path */

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
    /* Non-proxied settings, optional: called with the setting's index after
     * its value changed (cfg_init's default, set, reset, load), in the
     * caller's thread. The owner reads the value with cfg_u() / cfg_get(). */
    void (*notify)(uint8_t i);
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

/* Lock around list copies (cfg_get, cfg_set, cfg_load of CFG_LIST values
 * only, held for the copy): take returns the key that give gets back.
 * NULL (the default): no lock. */
void cfg_set_lock(uint32_t (*take)(void), void (*give)(uint32_t key));

/* Validate the table, take it with its storage, set every non-proxied value
 * to its default and then notify its owner. vals[n] holds the scalar values
 * (for a list: its slot number), lists[n_lists] one slot per CFG_LIST def,
 * assigned in table order.
 * 0, or -EINVAL with the previous table kept in use: more than CFG_MAX
 * settings, more lists than slots, a key that is NULL, empty, longer than
 * CFG_KEY_MAX or used twice, a proxied setting without get (or without set
 * unless read-only), a read-only setting that is not proxied, a proxied
 * list, an enum or list without names, a list with more than CFG_LIST_MAX
 * names, a uint range with min > max, an unknown type, or a default the
 * setting's own checks refuse. */
int cfg_init(const struct cfg_def *defs, uint8_t n, uint32_t *vals, struct cfg_list_slot *lists,
             uint8_t n_lists);

uint8_t cfg_count(void);
const struct cfg_def *cfg_def(uint8_t i); /* NULL past the end */
int cfg_find(const char *key, size_t len); /* index, or -ENOENT */
uint8_t cfg_name_count(const struct cfg_def *d); /* 0 for NULL or no names */
int cfg_name_find(const struct cfg_def *d, const char *s, size_t len); /* index, or -EINVAL */

/* The value of setting i; all zero (an empty list) past the end. */
void cfg_get(uint8_t i, struct cfg_value *out);
/* Scalar value (bool, uint, enum index, colour) for owners; 0 past the end. */
uint32_t cfg_u(uint8_t i);

/* Validate, normalize and apply; on success bump rev, mark a non-proxied
 * setting dirty, notify its owner and copy the value as stored to *stored
 * (may be NULL). 0, -ENOENT (no such index), -EINVAL, -EACCES (read-only). */
int cfg_set(uint8_t i, const struct cfg_value *in, struct cfg_value *stored);
/* Back to the default, like cfg_set(): 0, -ENOENT or -EACCES (-EINVAL only
 * for a default that cfg_init() would have refused). */
int cfg_reset(uint8_t i);
/* The store's path: validate and apply like cfg_set() and notify the owner,
 * without rev and dirty. -EACCES for proxied and read-only settings. */
int cfg_load(uint8_t i, const struct cfg_value *in);

/* True if setting i holds its default (the store keeps no entry then);
 * false past the end. */
bool cfg_is_default(uint8_t i);

/* Fn+Enter on list setting i: the name index after cur in the list
 * (wrapping), the first entry if cur is not listed, and with an empty list
 * the next name in table order ((cur + 1) % number of names). -ENOENT if i
 * is not a list setting. */
int cfg_cycle_next(uint8_t i, uint8_t cur);

uint32_t cfg_rev(void);
void cfg_note_change(void);         /* owners: a proxied value changed elsewhere */
uint32_t cfg_take_dirty(void);      /* bit i: setting i changed since the last call */
void cfg_mark_dirty(uint32_t mask); /* the store: save these again (retry), no rev change */

#endif /* RAINY75_CFG_REGISTRY_H */
