/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub (test_cfg_table.c): log calls go to test_log(), which the test
 * defines (it counts the errors and keeps the last message).
 */

#ifndef STUB_ZEPHYR_LOGGING_LOG_H
#define STUB_ZEPHYR_LOGGING_LOG_H

void test_log(const char *fmt, ...);

#define LOG_MODULE_REGISTER(name, level) void test_log(const char *fmt, ...)
#define LOG_ERR(...) test_log(__VA_ARGS__)
#define LOG_WRN(...) test_log(__VA_ARGS__)
#define LOG_INF(...) ((void)0)
#define LOG_DBG(...) ((void)0)

#endif
