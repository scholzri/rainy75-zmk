/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Host stub (test_cfg_table.c): a spinlock that does nothing, with the
 * shape of Zephyr's (k_spinlock_key_t has an int key).
 */

#ifndef STUB_ZEPHYR_SPINLOCK_H
#define STUB_ZEPHYR_SPINLOCK_H

struct k_spinlock {
    int unused;
};

typedef struct {
    int key;
} k_spinlock_key_t;

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l) {
    (void)l;
    return (k_spinlock_key_t){.key = 0};
}

static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key) {
    (void)l;
    (void)key;
}

#endif
