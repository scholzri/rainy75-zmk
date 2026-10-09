/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host tests for the stored form of a setting (cfg_codec.c).
 */

#include "../cfg_codec.h"
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
    static const char *const n[] = {"a", "bb", "ccc"};
    return i < 3 ? n[i] : NULL;
}

/* 20 names, more than one list value holds. */
static const char *twenty(uint8_t i) {
    static const char *const n[] = {"a", "b", "c", "d", "e", "f", "g", "h", "i", "j",
                                    "k", "l", "m", "n", "o", "p", "q", "r", "s", "t"};
    return i < 20 ? n[i] : NULL;
}

static const struct cfg_def D_BOOL = {.key = "t.b", .type = CFG_BOOL};
static const struct cfg_def D_UINT = {.key = "t.u", .type = CFG_UINT, .max = 0xFFFFFFFFu};
static const struct cfg_def D_COL = {.key = "t.c", .type = CFG_COLOR};
static const struct cfg_def D_ENUM = {.key = "t.e", .type = CFG_ENUM, .names = abc};
static const struct cfg_def D_LIST = {.key = "t.l", .type = CFG_LIST, .names = abc};
static const struct cfg_def D_MANY = {.key = "t.m", .type = CFG_LIST, .names = twenty};

static void test_scalars(void) {
    char buf[CFG_CODEC_BUF];
    struct cfg_value v = {.u = 0x01020304u}, out;

    CHECK(cfg_codec_encode(&D_UINT, &v, buf, sizeof(buf)) == 4);
    CHECK(memcmp(buf, "\x04\x03\x02\x01", 4) == 0); /* little-endian */
    CHECK(cfg_codec_decode(&D_UINT, buf, 4, &out) == 0 && out.u == 0x01020304u);
    v.u = 0xFF8000;
    CHECK(cfg_codec_encode(&D_COL, &v, buf, sizeof(buf)) == 4);
    CHECK(memcmp(buf, "\x00\x80\xFF\x00", 4) == 0);
    CHECK(cfg_codec_decode(&D_COL, buf, 4, &out) == 0 && out.u == 0xFF8000);
    v.u = 1;
    CHECK(cfg_codec_encode(&D_BOOL, &v, buf, sizeof(buf)) == 4);
    CHECK(cfg_codec_decode(&D_BOOL, buf, 4, &out) == 0 && out.u == 1);
    CHECK(cfg_codec_decode(&D_UINT, buf, 3, &out) == -EINVAL); /* wrong length */
    CHECK(cfg_codec_encode(&D_UINT, &v, buf, 3) == -ENOMEM);
}

static void test_enum(void) {
    char buf[CFG_CODEC_BUF];
    struct cfg_value v = {.u = 1}, out;

    CHECK(cfg_codec_encode(&D_ENUM, &v, buf, sizeof(buf)) == 2 && memcmp(buf, "bb", 2) == 0);
    CHECK(cfg_codec_decode(&D_ENUM, "ccc", 3, &out) == 0 && out.u == 2);
    CHECK(cfg_codec_decode(&D_ENUM, "dd", 2, &out) == -EINVAL); /* unknown: default kept */
    v.u = 3;
    CHECK(cfg_codec_encode(&D_ENUM, &v, buf, sizeof(buf)) == -EINVAL); /* no such name */
    v.u = 1;
    CHECK(cfg_codec_encode(&D_ENUM, &v, buf, 1) == -ENOMEM);
}

static void test_list(void) {
    char buf[CFG_CODEC_BUF];
    struct cfg_value v = {.len = 2, .idx = {2, 0}}, out;

    CHECK(cfg_codec_encode(&D_LIST, &v, buf, sizeof(buf)) == 5 && memcmp(buf, "ccc,a", 5) == 0);
    CHECK(cfg_codec_decode(&D_LIST, buf, 5, &out) == 0);
    CHECK(out.len == 2 && out.idx[0] == 2 && out.idx[1] == 0);
    CHECK(cfg_codec_decode(&D_LIST, "a,zz,bb", 7, &out) == 0); /* unknown names skipped */
    CHECK(out.len == 2 && out.idx[0] == 0 && out.idx[1] == 1);
    CHECK(cfg_codec_encode(&D_LIST, &v, buf, 4) == -ENOMEM);
}

static void test_empty_list(void) {
    char buf[CFG_CODEC_BUF];
    struct cfg_value v = {.len = 0}, out;

    /* stored as "," (length 0 would delete the entry and load the default) */
    CHECK(cfg_codec_encode(&D_LIST, &v, buf, sizeof(buf)) == 1 && buf[0] == ',');
    out.len = 9;
    CHECK(cfg_codec_decode(&D_LIST, buf, 1, &out) == 0 && out.len == 0);
    CHECK(cfg_codec_decode(&D_LIST, "zz,yy", 5, &out) == 0 && out.len == 0); /* only unknown */
}

static void test_list_cap(void) {
    const char *s = "a,b,c,d,e,f,g,h,i,j,k,l,m,n,o,p,q"; /* 17 known names */
    struct cfg_value out;

    CHECK(cfg_codec_decode(&D_MANY, s, strlen(s), &out) == 0);
    CHECK(out.len == CFG_LIST_MAX && out.idx[CFG_LIST_MAX - 1] == CFG_LIST_MAX - 1);
}

int main(void) {
    test_scalars();
    test_enum();
    test_list();
    test_empty_list();
    test_list_cap();
    if (failed) {
        printf("%d check(s) failed\n", failed);
        return 1;
    }
    printf("cfg_codec: all tests passed\n");
    return 0;
}
