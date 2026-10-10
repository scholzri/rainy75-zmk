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

/* 17 names: one more than a list setting may have. */
static const char *seventeen(uint8_t i) {
    static const char *const n[] = {"n0", "n1",  "n2",  "n3",  "n4",  "n5",  "n6",  "n7", "n8",
                                    "n9", "n10", "n11", "n12", "n13", "n14", "n15", "n16"};
    return i < 17 ? n[i] : NULL;
}

/* The owner of the proxied settings: values by arg. */
static uint32_t proxy_val[4];
static int proxy_sets;

static uint32_t proxy_get(uint8_t arg) { return proxy_val[arg]; }

static void proxy_set(uint8_t arg, uint32_t v) {
    proxy_val[arg] = v;
    proxy_sets++;
}

/* The owners of the other settings: change notifications by index, and the
 * value a scalar setting's owner read with cfg_u() when it was notified (not
 * for the list: test_list_lock counts its lock calls). */
static int notified[8];
static uint32_t notified_u[8];

static void note(uint8_t i) {
    notified[i]++;
    if (cfg_def(i)->type != CFG_LIST) {
        notified_u[i] = cfg_u(i);
    }
}

/* A lock that counts and checks the key it hands out. */
static int locks, unlocks, bad_keys;

static uint32_t fake_take(void) {
    locks++;
    return 0x5Au;
}

static void fake_give(uint32_t key) {
    unlocks++;
    bad_keys += (key != 0x5Au);
}

enum { T_FLAG, T_NUM, T_MODE, T_COL, T_LIST, T_PROXY, T_RO, T_N };

static const struct cfg_def defs[T_N] = {
    [T_FLAG] = {.key = "t.flag", .type = CFG_BOOL, .def = 1},
    [T_NUM] = {.key = "t.num", .type = CFG_UINT, .min = 10, .max = 20, .def = 15, .notify = note},
    [T_MODE] = {.key = "t.mode", .type = CFG_ENUM, .def = 1, .names = abc},
    [T_COL] = {.key = "t.col", .type = CFG_COLOR, .def = 0x123456, .notify = note},
    [T_LIST] = {.key = "t.list", .type = CFG_LIST, .names = xyz, .notify = note},
    [T_PROXY] = {.key = "t.proxy",
                 .type = CFG_UINT,
                 .flags = CFG_F_PROXY,
                 .max = 255,
                 .def = 7,
                 .get = proxy_get,
                 .set = proxy_set,
                 .arg = 2,
                 .notify = note}, /* never called: proxied */
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
    memset(notified, 0, sizeof(notified));
    memset(notified_u, 0, sizeof(notified_u));
    cfg_set_lock(NULL, NULL);
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
    CHECK(cfg_name_count(NULL) == 0);
    CHECK(cfg_name_find(&defs[T_MODE], "c", 1) == 2);
    CHECK(cfg_name_find(&defs[T_MODE], "d", 1) == -EINVAL);
    CHECK(cfg_name_find(&defs[T_MODE], "ab", 1) == 0); /* by length, not NUL */
    CHECK(cfg_name_find(NULL, "a", 1) == -EINVAL);
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
        {.key = "a", .type = CFG_LIST, .flags = CFG_F_PROXY, .names = xyz, .get = proxy_get,
         .set = proxy_set},
    };
    uint32_t v[2];
    struct cfg_list_slot s[1];

    CHECK(cfg_init(two_lists, 2, v, s, 1) == -EINVAL);
    CHECK(cfg_init(proxy_list, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(defs, CFG_MAX + 1, vals, lists, 1) == -EINVAL);
}

/* Every table rule of cfg_init(), one bad def at a time. */
static void test_validation(void) {
    static const struct cfg_def long_key[] = {
        {.key = "abcdefghijklmnopqrstuvwxyz012345", .type = CFG_BOOL}}; /* 32 characters */
    static const struct cfg_def max_key[] = {
        {.key = "abcdefghijklmnopqrstuvwxyz01234", .type = CFG_BOOL}}; /* 31 characters */
    static const struct cfg_def empty_key[] = {{.key = "", .type = CFG_BOOL}};
    static const struct cfg_def null_key[] = {{.type = CFG_BOOL}};
    static const struct cfg_def dup_key[] = {{.key = "a", .type = CFG_BOOL},
                                             {.key = "a", .type = CFG_UINT, .max = 1}};
    static const struct cfg_def proxy_no_get[] = {
        {.key = "a", .type = CFG_UINT, .flags = CFG_F_PROXY, .max = 9, .set = proxy_set}};
    static const struct cfg_def proxy_no_set[] = {
        {.key = "a", .type = CFG_UINT, .flags = CFG_F_PROXY, .max = 9, .get = proxy_get}};
    static const struct cfg_def ro_not_proxy[] = {
        {.key = "a", .type = CFG_UINT, .flags = CFG_F_RO, .max = 9}};
    static const struct cfg_def ro_get_only[] = {
        {.key = "a", .type = CFG_UINT, .flags = CFG_F_PROXY | CFG_F_RO, .max = 9,
         .get = proxy_get}};
    static const struct cfg_def enum_no_names[] = {{.key = "a", .type = CFG_ENUM}};
    static const struct cfg_def enum_bad_def[] = {
        {.key = "a", .type = CFG_ENUM, .def = 3, .names = abc}};
    static const struct cfg_def uint_min_max[] = {
        {.key = "a", .type = CFG_UINT, .min = 5, .max = 4, .def = 5}};
    static const struct cfg_def uint_bad_def[] = {
        {.key = "a", .type = CFG_UINT, .min = 5, .max = 9, .def = 4}};
    static const struct cfg_def bool_bad_def[] = {{.key = "a", .type = CFG_BOOL, .def = 2}};
    static const struct cfg_def colour_bad_def[] = {
        {.key = "a", .type = CFG_COLOR, .def = 0x1000000}};
    static const struct cfg_def list_no_names[] = {{.key = "a", .type = CFG_LIST}};
    static const struct cfg_def list_too_many[] = {
        {.key = "a", .type = CFG_LIST, .names = seventeen}};
    static const struct cfg_def bad_type[] = {{.key = "a", .type = 'x'}};
    uint32_t v[2];
    struct cfg_list_slot s[1];

    CHECK(cfg_init(long_key, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(empty_key, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(null_key, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(dup_key, 2, v, s, 1) == -EINVAL);
    CHECK(cfg_init(proxy_no_get, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(proxy_no_set, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(ro_not_proxy, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(enum_no_names, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(enum_bad_def, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(uint_min_max, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(uint_bad_def, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(bool_bad_def, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(colour_bad_def, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(list_no_names, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(list_too_many, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(bad_type, 1, v, s, 1) == -EINVAL);
    CHECK(cfg_init(max_key, 1, v, s, 1) == 0);
    CHECK(cfg_init(ro_get_only, 1, v, s, 1) == 0);
}

static void test_failed_init_keeps_registry(void) {
    static const struct cfg_def bad[] = {{.key = "a", .type = CFG_ENUM}};
    uint32_t v[1];
    struct cfg_value x = U(12);

    setup();
    CHECK(cfg_set(T_NUM, &x, NULL) == 0);
    CHECK(cfg_init(bad, 1, v, lists, 1) == -EINVAL);
    CHECK(cfg_count() == T_N && cfg_find("t.num", 5) == T_NUM && cfg_u(T_NUM) == 12);
    CHECK(cfg_rev() == 1); /* not restarted either */
}

static void test_index_guards(void) {
    struct cfg_value v;

    setup();
    memset(&v, 0xAA, sizeof(v));
    cfg_get(T_N, &v);
    CHECK(v.u == 0 && v.len == 0);
    CHECK(cfg_u(200) == 0);
    CHECK(!cfg_is_default(T_N));
    CHECK(cfg_cycle_next(T_N, 0) == -ENOENT);
    CHECK(cfg_cycle_next(T_NUM, 0) == -ENOENT); /* not a list */
    CHECK(cfg_reset(T_N) == -ENOENT && cfg_load(T_N, &v) == -ENOENT);
}

static void test_notify(void) {
    struct cfg_value v = U(12), bad = U(30), l = {.len = 1, .idx = {2}};

    setup();
    /* init reports every non-proxied default to its owner */
    CHECK(notified[T_NUM] == 1 && notified[T_COL] == 1 && notified[T_LIST] == 1);
    CHECK(cfg_set(T_NUM, &v, NULL) == 0 && notified[T_NUM] == 2);
    CHECK(notified_u[T_NUM] == 12); /* the owner reads the new value in its callback */
    CHECK(cfg_set(T_NUM, &bad, NULL) == -EINVAL && notified[T_NUM] == 2); /* refused: no news */
    CHECK(cfg_load(T_NUM, &v) == 0 && notified[T_NUM] == 3);
    CHECK(cfg_reset(T_NUM) == 0 && notified[T_NUM] == 4);
    CHECK(cfg_set(T_LIST, &l, NULL) == 0 && notified[T_LIST] == 2);
    v = U(99);
    CHECK(cfg_set(T_PROXY, &v, NULL) == 0 && notified[T_PROXY] == 0); /* the owner set it itself */
}

static void test_list_lock(void) {
    struct cfg_value l = {.len = 2, .idx = {2, 0}}, out;

    setup();
    locks = unlocks = bad_keys = 0;
    cfg_set_lock(fake_take, fake_give);
    CHECK(cfg_set(T_LIST, &l, &out) == 0); /* copy in, and out for *stored */
    CHECK(locks == 2 && unlocks == 2 && bad_keys == 0);
    cfg_get(T_LIST, &out);
    CHECK(locks == 3 && unlocks == 3);
    (void)cfg_u(T_NUM); /* scalars: no lock */
    cfg_get(T_COL, &out);
    CHECK(locks == 3);
    cfg_set_lock(NULL, NULL);
}

static void test_is_default(void) {
    struct cfg_value v = U(11), l = {.len = 2, .idx = {2, 0}};
    struct cfg_value all = {.len = 3, .idx = {0, 1, 2}}, empty = {.len = 0};

    setup();
    CHECK(cfg_is_default(T_NUM) && cfg_is_default(T_LIST) && cfg_is_default(T_COL));
    CHECK(cfg_set(T_NUM, &v, NULL) == 0 && !cfg_is_default(T_NUM));
    CHECK(cfg_reset(T_NUM) == 0 && cfg_is_default(T_NUM));
    CHECK(cfg_set(T_LIST, &l, NULL) == 0 && !cfg_is_default(T_LIST));
    CHECK(cfg_set(T_LIST, &all, NULL) == 0 && cfg_is_default(T_LIST)); /* all names in order */
    CHECK(cfg_set(T_LIST, &empty, NULL) == 0 && !cfg_is_default(T_LIST)); /* [] is not it */
}

static void test_cycle(void) {
    struct cfg_value l = {.len = 2, .idx = {2, 0}}, one = {.len = 1, .idx = {1}};
    struct cfg_value empty = {.len = 0};

    setup(); /* default: all names in order (x, y, z) */
    CHECK(cfg_cycle_next(T_LIST, 0) == 1);
    CHECK(cfg_cycle_next(T_LIST, 2) == 0); /* wraps */
    CHECK(cfg_set(T_LIST, &l, NULL) == 0); /* z, x */
    CHECK(cfg_cycle_next(T_LIST, 2) == 0);
    CHECK(cfg_cycle_next(T_LIST, 0) == 2);
    CHECK(cfg_cycle_next(T_LIST, 1) == 2); /* current not listed: the first entry */
    CHECK(cfg_set(T_LIST, &one, NULL) == 0);
    CHECK(cfg_cycle_next(T_LIST, 1) == 1 && cfg_cycle_next(T_LIST, 0) == 1);
    CHECK(cfg_set(T_LIST, &empty, NULL) == 0); /* empty: all names in table order */
    CHECK(cfg_cycle_next(T_LIST, 1) == 2 && cfg_cycle_next(T_LIST, 2) == 0);
}

static void test_mark_dirty(void) {
    setup();
    CHECK(cfg_take_dirty() == 0);
    cfg_mark_dirty((1u << T_COL) | (1u << T_NUM));
    CHECK(cfg_take_dirty() == ((1u << T_COL) | (1u << T_NUM)));
    CHECK(cfg_rev() == 0); /* a retry is not a change */
}

static void test_list_load_and_refusal(void) {
    struct cfg_value in = {.len = 2, .idx = {1, 0}}, ok = {.len = 1, .idx = {2}};
    struct cfg_value bad = {.len = 2, .idx = {0, 3}}, out;

    setup();
    CHECK(cfg_load(T_LIST, &in) == 0);
    cfg_get(T_LIST, &out);
    CHECK(out.len == 2 && out.idx[0] == 1 && out.idx[1] == 0);
    CHECK(cfg_rev() == 0 && cfg_take_dirty() == 0);
    CHECK(cfg_set(T_LIST, &ok, NULL) == 0);
    CHECK(cfg_set(T_LIST, &bad, NULL) == -EINVAL);
    cfg_get(T_LIST, &out);
    CHECK(out.len == 1 && out.idx[0] == 2); /* unchanged by the refused set */
}

static void test_proxy_reset_and_range(void) {
    struct cfg_value v = U(256);

    setup();
    CHECK(cfg_set(T_PROXY, &v, NULL) == -EINVAL && proxy_sets == 0); /* max 255 */
    v = U(9);
    CHECK(cfg_set(T_PROXY, &v, NULL) == 0);
    cfg_take_dirty();
    CHECK(cfg_reset(T_PROXY) == 0 && proxy_val[2] == 7);
    CHECK(cfg_rev() == 2 && cfg_take_dirty() == 0); /* a change, the owner persists it */
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
    test_validation();
    test_failed_init_keeps_registry();
    test_index_guards();
    test_notify();
    test_list_lock();
    test_is_default();
    test_cycle();
    test_mark_dirty();
    test_list_load_and_refusal();
    test_proxy_reset_and_range();
    if (failed) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("cfg_registry: all tests passed\n");
    return 0;
}
