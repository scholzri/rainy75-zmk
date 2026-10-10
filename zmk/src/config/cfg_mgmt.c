/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * mcumgr group 67 (MGMT_GROUP_ID_PERUSER + 3): runtime settings, see
 * docs/config-protocol.md.
 *   0 read  info  {}                 -> {rc, v, n, fx, rev}
 *   1 read  list  {i?}               -> {rc, s: [[key, type, a, b, flags]...], next?}
 *   2 read  get   {i?} or {k: [...], i?} -> {rc, v: {key: value...}, next?}
 *   3 write set   {k, v}             -> {rc, v (as stored)}
 *   4 write reset {k?: [...]}        -> {rc}
 * list and get page so that every reply fits one mcumgr buffer (512 bytes):
 * the size of each entry is estimated generously, a page holds entries up
 * to CFG_PAGE_BUDGET bytes, and at least one. get pages by key too: with k,
 * i indexes into k (a key listed twice is answered once); the client
 * continues with the same request and i = next.
 */

#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>

#include "cfg_registry.h"
#include "cfg_store.h"
#include "cfg_table.h"

LOG_MODULE_REGISTER(cfg_mgmt, LOG_LEVEL_INF);

#define CFG_MGMT_GROUP_ID (MGMT_GROUP_ID_PERUSER + 3)
#define CFG_MGMT_ID_INFO 0
#define CFG_MGMT_ID_LIST 1
#define CFG_MGMT_ID_GET 2
#define CFG_MGMT_ID_SET 3
#define CFG_MGMT_ID_RESET 4

#define CFG_PROTO_VERSION 1
#define CFG_PAGE_BUDGET 360

static int to_mgmt(int rc) {
    switch (rc) {
    case 0: return MGMT_ERR_EOK;
    case -ENOENT: return MGMT_ERR_ENOENT;
    case -EACCES: return MGMT_ERR_EACCESSDENIED;
    default: return MGMT_ERR_EINVAL;
    }
}

static bool put_str(zcbor_state_t *zse, const char *s) {
    return zcbor_tstr_encode_ptr(zse, s, strlen(s));
}

static bool put_names(zcbor_state_t *zse, const struct cfg_def *d) {
    uint8_t n = cfg_name_count(d);
    bool ok = zcbor_list_start_encode(zse, UINT8_MAX);

    for (uint8_t j = 0; ok && j < n; j++) {
        ok = put_str(zse, d->names(j));
    }
    return ok && zcbor_list_end_encode(zse, UINT8_MAX);
}

static bool put_value(zcbor_state_t *zse, const struct cfg_def *d, const struct cfg_value *v) {
    switch (d->type) {
    case CFG_BOOL:
        return zcbor_bool_put(zse, v->u != 0);
    case CFG_UINT:
    case CFG_COLOR:
        return zcbor_uint32_put(zse, v->u);
    case CFG_ENUM:
        return put_str(zse, d->names((uint8_t)v->u));
    case CFG_LIST: {
        bool ok = zcbor_list_start_encode(zse, CFG_LIST_MAX);

        for (uint8_t j = 0; ok && j < v->len; j++) {
            ok = put_str(zse, d->names(v->idx[j]));
        }
        return ok && zcbor_list_end_encode(zse, CFG_LIST_MAX);
    }
    default:
        return false;
    }
}

/* Generous CBOR size estimates for paging. */
static size_t names_size(const struct cfg_def *d) {
    size_t s = 2;

    for (uint8_t j = 0; j < cfg_name_count(d); j++) {
        s += 2 + strlen(d->names(j));
    }
    return s;
}

static size_t entry_size(const struct cfg_def *d) {
    size_t s = 2 + (2 + strlen(d->key)) + 2 + 2; /* list, key, type, flags */

    if (d->type == CFG_UINT) {
        s += 10;
    } else if (d->type == CFG_ENUM || d->type == CFG_LIST) {
        s += names_size(d) + 1;
    } else {
        s += 2;
    }
    return s;
}

static size_t value_size(const struct cfg_def *d, const struct cfg_value *v) {
    size_t s = 2 + strlen(d->key);

    switch (d->type) {
    case CFG_ENUM:
        return s + 2 + strlen(d->names((uint8_t)v->u));
    case CFG_LIST:
        s += 2;
        for (uint8_t j = 0; j < v->len; j++) {
            s += 2 + strlen(d->names(v->idx[j]));
        }
        return s;
    default:
        return s + 5;
    }
}

/* Any request value: bool, uint, text or a list of texts. */
struct any_val {
    char kind; /* 'b', 'u', 't', 'l' */
    bool b;
    uint32_t u;
    struct zcbor_string s;
    uint8_t n;
    struct zcbor_string items[CFG_LIST_MAX];
};

static bool decode_any(zcbor_state_t *zsd, void *result) {
    struct any_val *a = result;

    if (zsd->payload >= zsd->payload_end) {
        return false;
    }
    switch (ZCBOR_MAJOR_TYPE(*zsd->payload)) {
    case ZCBOR_MAJOR_TYPE_PINT:
        a->kind = 'u';
        return zcbor_uint32_decode(zsd, &a->u);
    case ZCBOR_MAJOR_TYPE_SIMPLE:
        a->kind = 'b';
        return zcbor_bool_decode(zsd, &a->b);
    case ZCBOR_MAJOR_TYPE_TSTR:
        a->kind = 't';
        return zcbor_tstr_decode(zsd, &a->s);
    case ZCBOR_MAJOR_TYPE_LIST:
        a->kind = 'l';
        a->n = 0;
        if (!zcbor_list_start_decode(zsd)) {
            return false;
        }
        while (!zcbor_array_at_end(zsd)) {
            if (a->n >= CFG_LIST_MAX || !zcbor_tstr_decode(zsd, &a->items[a->n])) {
                return false;
            }
            a->n++;
        }
        return zcbor_list_end_decode(zsd);
    default:
        return false;
    }
}

struct key_list {
    uint8_t n;
    struct zcbor_string k[CFG_MAX];
};

static bool decode_keys(zcbor_state_t *zsd, void *result) {
    struct key_list *kl = result;

    kl->n = 0;
    if (!zcbor_list_start_decode(zsd)) {
        return false;
    }
    while (!zcbor_array_at_end(zsd)) {
        if (kl->n >= CFG_MAX || !zcbor_tstr_decode(zsd, &kl->k[kl->n])) {
            return false;
        }
        kl->n++;
    }
    return zcbor_list_end_decode(zsd);
}

/* Request value -> cfg_value for d's type: -EINVAL on a type mismatch or an
 * unknown name. Range and duplicates are the registry's job. */
static int to_value(const struct cfg_def *d, const struct any_val *a, struct cfg_value *v) {
    memset(v, 0, sizeof(*v));
    switch (d->type) {
    case CFG_BOOL:
        if (a->kind != 'b') {
            return -EINVAL;
        }
        v->u = a->b;
        return 0;
    case CFG_UINT:
    case CFG_COLOR:
        if (a->kind != 'u') {
            return -EINVAL;
        }
        v->u = a->u;
        return 0;
    case CFG_ENUM: {
        int x;

        if (a->kind != 't') {
            return -EINVAL;
        }
        x = cfg_name_find(d, (const char *)a->s.value, a->s.len);
        if (x < 0) {
            return x;
        }
        v->u = x;
        return 0;
    }
    case CFG_LIST:
        if (a->kind != 'l') {
            return -EINVAL;
        }
        for (uint8_t j = 0; j < a->n; j++) {
            int x = cfg_name_find(d, (const char *)a->items[j].value, a->items[j].len);

            if (x < 0) {
                return x;
            }
            v->idx[v->len++] = x;
        }
        return 0;
    default:
        return -EINVAL;
    }
}

static int cfg_mgmt_info(struct smp_streamer *ctxt) {
    zcbor_state_t *zse = ctxt->writer->zs;
    bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0) &&
              zcbor_tstr_put_lit(zse, "v") && zcbor_uint32_put(zse, CFG_PROTO_VERSION) &&
              zcbor_tstr_put_lit(zse, "n") && zcbor_uint32_put(zse, cfg_count()) &&
              zcbor_tstr_put_lit(zse, "fx") && put_names(zse, cfg_def(CFG_RGB_EFFECT)) &&
              zcbor_tstr_put_lit(zse, "rev") && zcbor_uint32_put(zse, cfg_rev());

    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static bool put_entry(zcbor_state_t *zse, const struct cfg_def *d) {
    bool ok = zcbor_list_start_encode(zse, 5) && put_str(zse, d->key) &&
              zcbor_tstr_encode_ptr(zse, &d->type, 1);

    if (d->type == CFG_UINT) {
        ok = ok && zcbor_uint32_put(zse, d->min) && zcbor_uint32_put(zse, d->max);
    } else if (d->type == CFG_ENUM || d->type == CFG_LIST) {
        ok = ok && put_names(zse, d) && zcbor_nil_put(zse, NULL);
    } else {
        ok = ok && zcbor_nil_put(zse, NULL) && zcbor_nil_put(zse, NULL);
    }
    return ok && zcbor_uint32_put(zse, (d->flags & CFG_F_RO) ? 1 : 0) &&
           zcbor_list_end_encode(zse, 5);
}

static int cfg_mgmt_list(struct smp_streamer *ctxt) {
    zcbor_state_t *zse = ctxt->writer->zs;
    zcbor_state_t *zsd = ctxt->reader->zs;
    uint32_t start = 0, i;
    size_t decoded, used = 0;
    struct zcbor_map_decode_key_val map[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("i", zcbor_uint32_decode, &start),
    };
    bool ok;

    if (zcbor_map_decode_bulk(zsd, map, ARRAY_SIZE(map), &decoded) != 0) {
        return MGMT_ERR_EINVAL;
    }
    ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0) &&
         zcbor_tstr_put_lit(zse, "s") && zcbor_list_start_encode(zse, CFG_MAX);
    for (i = start; ok && i < cfg_count(); i++) {
        const struct cfg_def *d = cfg_def(i);
        size_t s = entry_size(d);

        if (i > start && used + s > CFG_PAGE_BUDGET) {
            break;
        }
        used += s;
        ok = put_entry(zse, d);
    }
    ok = ok && zcbor_list_end_encode(zse, CFG_MAX);
    if (ok && i < cfg_count()) {
        ok = zcbor_tstr_put_lit(zse, "next") && zcbor_uint32_put(zse, i);
    }
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

/* Encode setting i as key and value unless that would pass the page budget
 * (the first entry of a page always goes in): 1 encoded, 0 page full and
 * nothing encoded, -1 encoding failed. */
static int put_kv_paged(zcbor_state_t *zse, uint8_t i, size_t *used) {
    const struct cfg_def *d = cfg_def(i);
    struct cfg_value v;
    size_t s;

    cfg_get(i, &v);
    s = value_size(d, &v);
    if (*used > 0 && *used + s > CFG_PAGE_BUDGET) {
        return 0;
    }
    *used += s;
    return (put_str(zse, d->key) && put_value(zse, d, &v)) ? 1 : -1;
}

static int cfg_mgmt_get(struct smp_streamer *ctxt) {
    zcbor_state_t *zse = ctxt->writer->zs;
    zcbor_state_t *zsd = ctxt->reader->zs;
    uint32_t start = 0, pos, total;
    size_t decoded, used = 0;
    struct key_list kl = {0};
    uint8_t ids[CFG_MAX];
    struct zcbor_map_decode_key_val map[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("i", zcbor_uint32_decode, &start),
        ZCBOR_MAP_DECODE_KEY_DECODER("k", decode_keys, &kl),
    };
    bool by_key, ok;

    if (zcbor_map_decode_bulk(zsd, map, ARRAY_SIZE(map), &decoded) != 0) {
        return MGMT_ERR_EINVAL;
    }
    by_key = zcbor_map_decode_bulk_key_found(map, ARRAY_SIZE(map), "k");
    for (uint8_t j = 0; by_key && j < kl.n; j++) {
        int x = cfg_find((const char *)kl.k[j].value, kl.k[j].len);

        if (x < 0) {
            return MGMT_ERR_ENOENT;
        }
        ids[j] = x;
    }
    total = by_key ? kl.n : cfg_count();
    ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0) &&
         zcbor_tstr_put_lit(zse, "v") && zcbor_map_start_encode(zse, CFG_MAX);
    for (pos = start; ok && pos < total; pos++) {
        uint8_t id = by_key ? ids[pos] : pos;
        bool dup = false;
        int r;

        for (uint32_t j = 0; by_key && j < pos; j++) {
            dup = dup || ids[j] == id;
        }
        if (dup) {
            continue;
        }
        r = put_kv_paged(zse, id, &used);
        if (r == 0) {
            break;
        }
        ok = r > 0;
    }
    ok = ok && zcbor_map_end_encode(zse, CFG_MAX);
    if (ok && pos < total) {
        ok = zcbor_tstr_put_lit(zse, "next") && zcbor_uint32_put(zse, pos);
    }
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int cfg_mgmt_set(struct smp_streamer *ctxt) {
    zcbor_state_t *zse = ctxt->writer->zs;
    zcbor_state_t *zsd = ctxt->reader->zs;
    struct zcbor_string key = {0};
    struct any_val a = {0};
    struct cfg_value in, stored;
    size_t decoded;
    struct zcbor_map_decode_key_val map[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("k", zcbor_tstr_decode, &key),
        ZCBOR_MAP_DECODE_KEY_DECODER("v", decode_any, &a),
    };
    int i, rc;
    bool ok;

    if (zcbor_map_decode_bulk(zsd, map, ARRAY_SIZE(map), &decoded) != 0 ||
        !zcbor_map_decode_bulk_key_found(map, ARRAY_SIZE(map), "k") ||
        !zcbor_map_decode_bulk_key_found(map, ARRAY_SIZE(map), "v")) {
        return MGMT_ERR_EINVAL;
    }
    i = cfg_find((const char *)key.value, key.len);
    if (i < 0) {
        return MGMT_ERR_ENOENT;
    }
    if (cfg_def(i)->flags & CFG_F_RO) {
        return MGMT_ERR_EACCESSDENIED;
    }
    rc = to_value(cfg_def(i), &a, &in);
    if (rc == 0) {
        rc = cfg_set(i, &in, &stored);
    }
    if (rc != 0) {
        return to_mgmt(rc);
    }
    cfg_store_request_save();
    ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0) &&
         zcbor_tstr_put_lit(zse, "v") && put_value(zse, cfg_def(i), &stored);
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int cfg_mgmt_reset(struct smp_streamer *ctxt) {
    zcbor_state_t *zse = ctxt->writer->zs;
    zcbor_state_t *zsd = ctxt->reader->zs;
    struct key_list kl = {0};
    uint8_t ids[CFG_MAX];
    size_t decoded;
    struct zcbor_map_decode_key_val map[] = {
        ZCBOR_MAP_DECODE_KEY_DECODER("k", decode_keys, &kl),
    };
    bool ok;

    if (zcbor_map_decode_bulk(zsd, map, ARRAY_SIZE(map), &decoded) != 0) {
        return MGMT_ERR_EINVAL;
    }
    if (zcbor_map_decode_bulk_key_found(map, ARRAY_SIZE(map), "k")) {
        /* all named keys must exist and be writable before any is reset */
        for (uint8_t j = 0; j < kl.n; j++) {
            int x = cfg_find((const char *)kl.k[j].value, kl.k[j].len);

            if (x < 0) {
                return MGMT_ERR_ENOENT;
            }
            if (cfg_def(x)->flags & CFG_F_RO) {
                return MGMT_ERR_EACCESSDENIED;
            }
            ids[j] = x;
        }
        for (uint8_t j = 0; j < kl.n; j++) {
            (void)cfg_reset(ids[j]);
        }
    } else {
        for (uint8_t i = 0; i < cfg_count(); i++) {
            if (!(cfg_def(i)->flags & CFG_F_RO)) {
                (void)cfg_reset(i);
            }
        }
    }
    cfg_store_request_save();
    ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0);
    return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static const struct mgmt_handler cfg_mgmt_handlers[] = {
    [CFG_MGMT_ID_INFO] = {.mh_read = cfg_mgmt_info, .mh_write = NULL},
    [CFG_MGMT_ID_LIST] = {.mh_read = cfg_mgmt_list, .mh_write = NULL},
    [CFG_MGMT_ID_GET] = {.mh_read = cfg_mgmt_get, .mh_write = NULL},
    [CFG_MGMT_ID_SET] = {.mh_read = NULL, .mh_write = cfg_mgmt_set},
    [CFG_MGMT_ID_RESET] = {.mh_read = NULL, .mh_write = cfg_mgmt_reset},
};

static struct mgmt_group cfg_mgmt_group = {
    .mg_handlers = cfg_mgmt_handlers,
    .mg_handlers_count = ARRAY_SIZE(cfg_mgmt_handlers),
    .mg_group_id = CFG_MGMT_GROUP_ID,
};

static void cfg_mgmt_register(void) {
    mgmt_register_group(&cfg_mgmt_group);
    LOG_INF("cfg_mgmt registered (group %d)", CFG_MGMT_GROUP_ID);
}

MCUMGR_HANDLER_DEFINE(cfg_mgmt, cfg_mgmt_register);
