/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub (test_cfg_table.c): SYS_INIT hands the init function to the
 * test, which calls it as test_sys_init(). One SYS_INIT per test binary.
 */

#ifndef STUB_ZEPHYR_INIT_H
#define STUB_ZEPHYR_INIT_H

#define SYS_INIT(fn, level, prio) int (*const test_sys_init)(void) = fn

#endif
