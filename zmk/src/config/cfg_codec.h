/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Stored form of one setting (the rainy_cfg entries of cfg_store.c), pure
 * and host tested: bool, uint and colour as 4-byte little-endian, enum as
 * its name, list as its names joined with ','. An empty list is stored as a
 * single ',' (an entry of length 0 would be a deletion and load as the
 * default, all names); it decodes back to an empty list because empty names
 * are unknown. Names survive builds with a different effect set: decoding
 * an unknown enum name fails (the caller keeps the default), unknown list
 * names are skipped.
 */

#ifndef RAINY75_CFG_CODEC_H
#define RAINY75_CFG_CODEC_H

#include <stddef.h>

#include "cfg_registry.h"

#define CFG_CODEC_BUF 192 /* longest stored value: a full list of names */

/* buf[len] -> *v for d's type: 0, or -EINVAL (wrong length, unknown enum
 * name, unknown type). Not range checked: cfg_load() does that. */
int cfg_codec_decode(const struct cfg_def *d, const char *buf, size_t len, struct cfg_value *v);
/* *v -> buf[cap]: the number of bytes written (at least 1), or -ENOMEM (cap
 * too small) or -EINVAL (a name index without a name, unknown type). */
int cfg_codec_encode(const struct cfg_def *d, const struct cfg_value *v, char *buf, size_t cap);

#endif /* RAINY75_CFG_CODEC_H */
