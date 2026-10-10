/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub (test_cfg_table.c): the util.h macros the settings table uses.
 */

#ifndef STUB_ZEPHYR_SYS_UTIL_H
#define STUB_ZEPHYR_SYS_UTIL_H

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define ARG_UNUSED(x) (void)(x)
#define BUILD_ASSERT(cond, msg) _Static_assert(cond, msg)

#endif
