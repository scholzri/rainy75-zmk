/*
 * Copyright (c) 2022 Telink Semiconductor (Shanghai) Co., Ltd.
 * Copyright (c) 2025 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * BLE controller shim for Telink B91 (TLSR951x).
 * API-compatible with hal_telink's b91_bt.h, but compiled against
 * standard Zephyr headers to avoid SDK conflicts.
 */

#ifndef B91_BT_H_
#define B91_BT_H_

#include <stdint.h>

typedef struct b91_bt_host_callback {
	void (*host_send_available)(void);
	void (*host_read_packet)(uint8_t *data, uint16_t len);
} b91_bt_host_callback_t;

void b91_bt_host_callback_register(const b91_bt_host_callback_t *callback);
void b91_bt_host_send_packet(uint8_t type, uint8_t *data, uint16_t len);
int b91_bt_controller_init(void);

#endif /* B91_BT_H_ */
