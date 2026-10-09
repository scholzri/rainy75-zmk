/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the settings registry (cfg_registry.c), one test per rule
 * of cfg_registry.h.
 */

#include "../cfg_registry.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

static int failed;
#define CHECK(cond)                                                                                \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
            failed++;                                                                              \
        }                                                                                          \
    } while (0)

static const char *abc(uint8_t i) {
    static const char *const n[] = {"a", "b", "c"};
    return i < 3 ? n[i] : NULL;
}

static const char *xyz(uint8_t i) {
    static const char *const n[] = {"x", "y", "z"};
    return i < 3 ? n[i] : NULL;
}

/* The owner of the proxied settings: values by arg. */
static uint32_t proxy_val[4];
static int proxy_sets;

static uint32_t proxy_get(uint8_t arg) { return proxy_val[arg]; }

static void proxy_set(uint8_t arg, uint32_t v) {
    proxy_val[arg] = v;
    proxy_sets++;
}

enum { T_FLAG, T_NUM, T_MODE, T_COL, T_LIST, T_PROXY, T_RO, T_N };

static const struct cfg_def defs[T_N] = {
    [T_FLAG] = {.key = "t.flag", .type = CFG_BOOL, .def = 1},
    [T_NUM] = {.key = "t.num", .type = CFG_UINT, .min = 10, .max = 20, .def = 15},
    [T_MODE] = {.key = "t.mode", .type = CFG_ENUM, .def = 1, .names = abc},
    [T_COL] = {.key = "t.col", .type = CFG_COLOR, .def = 0x123456},
    [T_LIST] = {.key = "t.list", .type = CFG_LIST, .names = xyz},
    [T_PROXY] = {.key = "t.proxy",
                 .type = CFG_UINT,
                 .flags = CFG_F_PROXY,
                 .max = 255,
                 .def = 7,
                 .get = proxy_get,
                 .set = proxy_set,
                 .arg = 2},
    [T_RO] = {.key = "t.ro",
              .type = CFG_UINT,
              .flags = CFG_F_PROXY | CFG_F_RO,
              .max = 99,
              .get = proxy_get,
              .arg = 3},
};

static uint32_t vals[T_N];
static struct cfg_list_slot lists[1];

static void setup(void) {
    memset(proxy_val, 0, sizeof(proxy_val));
    proxy_val[2] = 42;
    proxy_val[3] = 5;
    proxy_sets = 0;
    CHECK(cfg_init(defs, T_N, vals, lists, 1) == 0);
}

static struct cfg_value U(uint32_t u) {
    struct cfg_value v = {.u = u};
    return v;
}

static void test_defaults(void) {
    struct cfg_value v;

    setup();
    CHECK(cfg_u(T_FLAG) == 1);
    CHECK(cfg_u(T_NUM) == 15);
    CHECK(cfg_u(T_MODE) == 1);
    CHECK(cfg_u(T_COL) == 0x123456);
    cfg_get(T_LIST, &v);
    CHECK(v.len == 3 && v.idx[0] == 0 && v.idx[1] == 1 && v.idx[2] == 2);
    /* proxied values belong to their owner: init leaves them alone */
    CHECK(proxy_sets == 0);
    CHECK(cfg_u(T_PROXY) == 42);
    CHECK(cfg_rev() == 0);
    CHECK(cfg_take_dirty() == 0);
}

static void test_find(void) {
    struct cfg_value v = U(1);

    setup();
    CHECK(cfg_find("t.num", 5) == T_NUM);
    CHECK(cfg_find("t.nu", 4) == -ENOENT);
    CHECK(cfg_find("t.numx", 6) == -ENOENT);
    CHECK(cfg_count() == T_N);
    CHECK(cfg_def(T_N) == NULL);
    CHECK(cfg_set(T_N, &v, NULL) == -ENOENT);
    CHECK(cfg_name_count(&defs[T_MODE]) == 3);
    CHECK(cfg_name_find(&defs[T_MODE], "c", 1) == 2);
    CHECK(cfg_name_find(&defs[T_MODE], "d", 1) == -EINVAL);
    CHECK(cfg_name_find(&defs[T_MODE], "ab", 1) == 0); /* by length, not NUL */
}

static void test_set_bool_and_rev(void) {
    struct cfg_value v = U(0), out;

    setup();
    CHECK(cfg_set(T_FLAG, &v, &out) == 0 && out.u == 0 && cfg_u(T_FLAG) == 0);
    CHECK(cfg_rev() == 1);
    CHECK(cfg_take_dirty() == (1u << T_FLAG));
    CHECK(cfg_take_dirty() == 0); /* taken */
    v = U(2);
    CHECK(cfg_set(T_FLAG, &v, NULL) == -EINVAL);
    CHECK(cfg_rev() == 1 && cfg_take_dirty() == 0); /* refused: no change */
}

static void test_uint_range(void) {
    struct cfg_value v;

    setup();
    v = U(10);
    CHECK(cfg_set(T_NUM, &v, NULL) == 0);
    v = U(20);
    CHECK(cfg_set(T_NUM, &v, NULL) == 0);
    v = U(9);
    CHECK(cfg_set(T_NUM, &v, NULL) == -EINVAL);
    v = U(21);
    CHECK(cfg_set(T_NUM, &v, NULL) == -EINVAL);
    CHECK(cfg_u(T_NUM) == 20);
}

static void test_enum_and_colour(void) {
    struct cfg_value v;

    setup();
    v = U(2);
    CHECK(cfg_set(T_MODE, &v, NULL) == 0 && cfg_u(T_MODE) == 2);
    v = U(3);
    CHECK(cfg_set(T_MODE, &v, NULL) == -EINVAL);
    v = U(0xFFFFFF);
    CHECK(cfg_set(T_COL, &v, NULL) == 0);
    v = U(0x1000000);
    CHECK(cfg_set(T_COL, &v, NULL) == -EINVAL);
    CHECK(cfg_u(T_COL) == 0xFFFFFF);
}

static void test_list_normalize(void) {
    struct cfg_value v = {.len = 3, .idx = {2, 0, 2}}, out;
    struct cfg_value bad = {.len = 1, .idx = {3}};
    struct cfg_value toolong = {.len = CFG_LIST_MAX + 1};
    struct cfg_value empty = {.len = 0};

    setup();
    CHECK(cfg_set(T_LIST, &v, &out) == 0);
    CHECK(out.len == 2 && out.idx[0] == 2 && out.idx[1] == 0); /* duplicate dropped, order kept */
    CHECK(cfg_set(T_LIST, &bad, NULL) == -EINVAL);
    CHECK(cfg_set(T_LIST, &toolong, NULL) == -EINVAL);
    CHECK(cfg_set(T_LIST, &empty, &out) == 0 && out.len == 0); /* owners treat empty as all */
}

static void test_proxy(void) {
    struct cfg_value v = U(99), out;

    setup();
    CHECK(cfg_set(T_PROXY, &v, &out) == 0);
    CHECK(proxy_sets == 1 && proxy_val[2] == 99 && out.u == 99);
    CHECK(cfg_rev() == 1);
    CHECK(cfg_take_dirty() == 0); /* the owner persists it */
    proxy_val[2] = 5;             /* the owner changed it (Fn key) */
    cfg_note_change();
    CHECK(cfg_u(T_PROXY) == 5 && cfg_rev() == 2);
}

static void test_read_only(void) {
    struct cfg_value v = U(1);

    setup();
    CHECK(cfg_u(T_RO) == 5);
    CHECK(cfg_set(T_RO, &v, NULL) == -EACCES);
    CHECK(cfg_reset(T_RO) == -EACCES);
    CHECK(cfg_load(T_RO, &v) == -EACCES);
    CHECK(cfg_rev() == 0);
}

static void test_reset(void) {
    struct cfg_value v = U(11), l = {.len = 1, .idx = {1}};

    setup();
    CHECK(cfg_set(T_NUM, &v, NULL) == 0 && cfg_set(T_LIST, &l, NULL) == 0);
    cfg_take_dirty();
    CHECK(cfg_reset(T_NUM) == 0 && cfg_u(T_NUM) == 15);
    CHECK(cfg_reset(T_LIST) == 0);
    cfg_get(T_LIST, &l);
    CHECK(l.len == 3);
    CHECK(cfg_take_dirty() == ((1u << T_NUM) | (1u << T_LIST)));
    CHECK(cfg_reset(T_PROXY) == 0 && proxy_val[2] == 7); /* proxied default through the hook */
}

static void test_load(void) {
    struct cfg_value v = U(12);

    setup();
    CHECK(cfg_load(T_NUM, &v) == 0 && cfg_u(T_NUM) == 12);
    CHECK(cfg_rev() == 0 && cfg_take_dirty() == 0); /* loading is not a change */
    v = U(30);
    CHECK(cfg_load(T_NUM, &v) == -EINVAL && cfg_u(T_NUM) == 12);
    v = U(1);
    CHECK(cfg_load(T_PROXY, &v) == -EACCES); /* the owner loads its own */
}

static void test_init_errors(void) {
    static const struct cfg_def two_lists[] = {
        {.key = "a", .type = CFG_LIST, .names = xyz},
        {.key = "b", .type = CFG_LIST, .names = xyz},
    };
    static const struct cfg_def proxy_list[] = {
        {.key = "a", .type = CFG_LIST, .flags = CFG_F_PROXY, .names = xyz},
    };
    uint32_t v[2];
    struct cfg_list_slot s[1];

    CHECK(cfg_init(two_lists, 2, v, s, 1) == -EINVAL);
    CHECK(cfg_init(proxy_list, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(defs, CFG_MAX + 1, vals, lists, 1) == -EINVAL);
}

int main(void) {
    test_defaults();
    test_find();
    test_set_bool_and_rev();
    test_uint_range();
    test_enum_and_colour();
    test_list_normalize();
    test_proxy();
    test_read_only();
    test_reset();
    test_load();
    test_init_errors();
    if (failed) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("cfg_registry: all tests passed\n");
    return 0;
}
