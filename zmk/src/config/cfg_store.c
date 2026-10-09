/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistence of the non-proxied settings: settings subtree "rainy_cfg",
 * one entry per key. bool, uint and colour as 4-byte little-endian, enum as
 * its name, list as names joined with ','. Names survive builds with a
 * different effect set: an unknown enum name keeps the default, unknown list
 * names are skipped. An invalid or unknown stored entry is logged and
 * ignored, never fatal. Saved 2 s after the last change, like the RGB state.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include "cfg_registry.h"
#include "cfg_store.h"
#include "cfg_table.h"

LOG_MODULE_REGISTER(cfg_store, CONFIG_LOG_DEFAULT_LEVEL);

#define CFG_STORE_DEBOUNCE_MS 2000
#define CFG_STORE_BUF 192 /* longest stored value: a full list of names */

static int decode(const struct cfg_def *d, const char *buf, size_t len, struct cfg_value *v) {
    memset(v, 0, sizeof(*v));
    switch (d->type) {
    case CFG_BOOL:
    case CFG_UINT:
    case CFG_COLOR:
        if (len != 4) {
            return -EINVAL;
        }
        v->u = sys_get_le32((const uint8_t *)buf);
        return 0;
    case CFG_ENUM: {
        int x = cfg_name_find(d, buf, len);

        if (x < 0) {
            return x;
        }
        v->u = x;
        return 0;
    }
    case CFG_LIST: {
        size_t start = 0;

        for (size_t p = 0; p <= len; p++) {
            if (p == len || buf[p] == ',') {
                int x = cfg_name_find(d, buf + start, p - start);

                if (x >= 0 && v->len < CFG_LIST_MAX) {
                    v->idx[v->len++] = x; /* unknown names skipped */
                }
                start = p + 1;
            }
        }
        return 0;
    }
    default:
        return -EINVAL;
    }
}

static int encode(const struct cfg_def *d, const struct cfg_value *v, char *buf, size_t cap) {
    switch (d->type) {
    case CFG_BOOL:
    case CFG_UINT:
    case CFG_COLOR:
        sys_put_le32(v->u, (uint8_t *)buf);
        return 4;
    case CFG_ENUM: {
        const char *n = d->names(v->u);
        size_t l = strlen(n);

        if (l > cap) {
            return -ENOMEM;
        }
        memcpy(buf, n, l);
        return l;
    }
    case CFG_LIST: {
        size_t o = 0;

        for (uint8_t j = 0; j < v->len; j++) {
            const char *n = d->names(v->idx[j]);
            size_t l = strlen(n);

            if (o + l + 1 > cap) {
                return -ENOMEM;
            }
            if (j > 0) {
                buf[o++] = ',';
            }
            memcpy(buf + o, n, l);
            o += l;
        }
        return o;
    }
    default:
        return -EINVAL;
    }
}

static int cfg_store_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    char buf[CFG_STORE_BUF];
    struct cfg_value v;
    int i = cfg_find(name, strlen(name));
    ssize_t rd;

    if (i < 0 || len > sizeof(buf)) {
        LOG_WRN("stored setting %s ignored", name);
        return 0;
    }
    rd = read_cb(cb_arg, buf, len);
    if (rd < 0) {
        return rd;
    }
    if (decode(cfg_def(i), buf, rd, &v) != 0 || cfg_load(i, &v) != 0) {
        LOG_WRN("stored setting %s invalid, default kept", name);
    }
    return 0;
}

static int cfg_store_commit(void) {
    cfg_table_loaded();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(rainy_cfg, "rainy_cfg", NULL, cfg_store_set, cfg_store_commit,
                               NULL);

static void save_work_fn(struct k_work *w) {
    uint32_t dirty = cfg_take_dirty();
    char buf[CFG_STORE_BUF];
    char path[48];

    ARG_UNUSED(w);
    for (uint8_t i = 0; i < cfg_count(); i++) {
        const struct cfg_def *d = cfg_def(i);
        struct cfg_value v;
        int n, rc;

        if (!(dirty & BIT(i))) {
            continue;
        }
        cfg_get(i, &v);
        n = encode(d, &v, buf, sizeof(buf));
        snprintk(path, sizeof(path), "rainy_cfg/%s", d->key);
        /* length 0 (an empty list) deletes the entry: loads as the default,
         * which owners treat the same as an empty list */
        rc = (n < 0) ? n : settings_save_one(path, buf, n);
        if (rc != 0) {
            LOG_ERR("save %s: %d", d->key, rc);
        }
    }
}

static K_WORK_DELAYABLE_DEFINE(save_work, save_work_fn);

void cfg_store_request_save(void) {
    k_work_reschedule(&save_work, K_MSEC(CFG_STORE_DEBOUNCE_MS));
}
