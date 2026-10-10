/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stored form of one setting, see cfg_codec.h. Pure: no Zephyr, no ZMK.
 */

#include "cfg_codec.h"

#include <errno.h>
#include <string.h>

static uint32_t get_le32(const char *b) {
    const uint8_t *p = (const uint8_t *)b;

    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void put_le32(uint32_t v, char *b) {
    uint8_t *p = (uint8_t *)b;

    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int cfg_codec_decode(const struct cfg_def *d, const char *buf, size_t len, struct cfg_value *v) {
    memset(v, 0, sizeof(*v));
    switch (d->type) {
    case CFG_BOOL:
    case CFG_UINT:
    case CFG_COLOR:
        if (len != 4) {
            return -EINVAL;
        }
        v->u = get_le32(buf);
        return 0;
    case CFG_ENUM: {
        int x = cfg_name_find(d, buf, len);

        if (x < 0) {
            return x;
        }
        v->u = (uint32_t)x;
        return 0;
    }
    case CFG_LIST: {
        size_t start = 0;

        for (size_t p = 0; p <= len; p++) {
            if (p == len || buf[p] == ',') {
                int x = cfg_name_find(d, buf + start, p - start);

                if (x >= 0 && v->len < CFG_LIST_MAX) {
                    v->idx[v->len++] = (uint8_t)x; /* unknown (and empty) names skipped */
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

int cfg_codec_encode(const struct cfg_def *d, const struct cfg_value *v, char *buf, size_t cap) {
    switch (d->type) {
    case CFG_BOOL:
    case CFG_UINT:
    case CFG_COLOR:
        if (cap < 4) {
            return -ENOMEM;
        }
        put_le32(v->u, buf);
        return 4;
    case CFG_ENUM: {
        const char *n = d->names != NULL ? d->names((uint8_t)v->u) : NULL;
        size_t l;

        if (n == NULL) {
            return -EINVAL;
        }
        l = strlen(n);
        if (l > cap) {
            return -ENOMEM;
        }
        memcpy(buf, n, l);
        return (int)l;
    }
    case CFG_LIST: {
        size_t o = 0;

        if (v->len == 0) {
            if (cap < 1) {
                return -ENOMEM;
            }
            buf[0] = ','; /* the empty list marker, see cfg_codec.h */
            return 1;
        }
        for (uint8_t j = 0; j < v->len; j++) {
            const char *n = d->names(v->idx[j]);
            size_t l;

            if (n == NULL) {
                return -EINVAL;
            }
            l = strlen(n);
            if (o + (j > 0 ? 1 : 0) + l > cap) {
                return -ENOMEM;
            }
            if (j > 0) {
                buf[o++] = ',';
            }
            memcpy(buf + o, n, l);
            o += l;
        }
        return (int)o;
    }
    default:
        return -EINVAL;
    }
}
