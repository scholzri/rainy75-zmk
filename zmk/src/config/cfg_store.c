/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Persistence of the non-proxied settings: settings subtree "rainy_cfg",
 * one entry per key, in the stored form of cfg_codec.c. A setting at its
 * default has no entry: saving it deletes the entry (so a reset deletes,
 * and a later firmware with a different default applies its new default).
 * An invalid or unknown stored entry is logged and ignored, never fatal.
 * Saved 2 s after the last change, like the RGB state; a failed save is
 * retried CFG_STORE_RETRIES times, CFG_STORE_RETRY_MS apart, then logged.
 *
 * Threads: cfg_store_set / commit run inside settings_load() (ZMK main
 * thread, at boot), the save on the system work queue. Values are read with
 * cfg_get() (lists under the registry lock), the dirty mask is atomic.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include "cfg_codec.h"
#include "cfg_registry.h"
#include "cfg_store.h"
#include "cfg_table.h"

LOG_MODULE_REGISTER(cfg_store, CONFIG_LOG_DEFAULT_LEVEL);

#define CFG_STORE_DEBOUNCE_MS 2000
#define CFG_STORE_RETRY_MS 10000
#define CFG_STORE_RETRIES 3
#define CFG_STORE_PREFIX "rainy_cfg/"

static int cfg_store_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    char buf[CFG_CODEC_BUF];
    struct cfg_value v;
    ssize_t rd;
    int i;

    if (name == NULL) { /* a stored key exactly "rainy_cfg": no remainder to match */
        LOG_WRN("stored setting without a name ignored");
        return 0;
    }
    i = cfg_find(name, strlen(name));
    if (i < 0 || len > sizeof(buf)) {
        LOG_WRN("stored setting %s ignored", name);
        return 0;
    }
    rd = read_cb(cb_arg, buf, len);
    if (rd < 0) {
        return rd;
    }
    if (cfg_codec_decode(cfg_def(i), buf, rd, &v) != 0 || cfg_load(i, &v) != 0) {
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

/* Save setting i, or delete its entry when it holds the default. */
static int save_one(uint8_t i) {
    const struct cfg_def *d = cfg_def(i);
    char path[sizeof(CFG_STORE_PREFIX) + CFG_KEY_MAX];
    char buf[CFG_CODEC_BUF];
    struct cfg_value v;
    int n;

    n = snprintk(path, sizeof(path), CFG_STORE_PREFIX "%s", d->key);
    if (n < 0 || n >= (int)sizeof(path)) {
        return -EINVAL; /* cfg_init() refuses keys this long */
    }
    if (cfg_is_default(i)) {
        return settings_delete(path); /* no entry: nothing written if there was none */
    }
    cfg_get(i, &v);
    n = cfg_codec_encode(d, &v, buf, sizeof(buf));
    return n < 0 ? n : settings_save_one(path, buf, n);
}

static uint8_t save_retries;

static void save_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(save_work, save_work_fn);

static void save_work_fn(struct k_work *w) {
    uint32_t dirty = cfg_take_dirty();
    uint32_t failed = 0;

    ARG_UNUSED(w);
    for (uint8_t i = 0; i < cfg_count(); i++) {
        int rc;

        if (!(dirty & BIT(i))) {
            continue;
        }
        rc = save_one(i);
        if (rc != 0) {
            LOG_ERR("save %s: %d", cfg_def(i)->key, rc);
            failed |= BIT(i);
        }
    }
    if (failed == 0) {
        save_retries = 0;
    } else if (save_retries < CFG_STORE_RETRIES) {
        save_retries++;
        cfg_mark_dirty(failed);
        k_work_reschedule(&save_work, K_MSEC(CFG_STORE_RETRY_MS));
    } else {
        LOG_ERR("settings 0x%08x not saved after %d retries", failed, CFG_STORE_RETRIES);
        save_retries = 0;
    }
}

void cfg_store_request_save(void) {
    k_work_reschedule(&save_work, K_MSEC(CFG_STORE_DEBOUNCE_MS));
}
