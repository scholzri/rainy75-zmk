/*
 * Copyright (c) 2025 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * Bluetooth HCI driver for Telink B91 (TLSR951x).
 *
 * Zephyr v4.x DEVICE_API wrapper that bridges b91_bt shim's callback
 * API to Zephyr's bt_hci driver interface.
 */

#include <zephyr/init.h>
#include <zephyr/sys/util.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/drivers/bluetooth.h>
#include <zephyr/kernel.h>

#define LOG_LEVEL CONFIG_BT_HCI_DRIVER_LOG_LEVEL
#include <zephyr/logging/log.h>
LOG_MODULE_REGISTER(bt_hci_b91);

#include "b91_bt.h"

#define DT_DRV_COMPAT telink_b91_bt_hci

struct hci_b91_data {
	bt_hci_recv_t recv;
};

/* --------------------------------------------------------------------------
 * HCI event/data receive path (controller -> host)
 * -------------------------------------------------------------------------- */

static bool is_hci_event_discardable(const uint8_t *evt_data)
{
	uint8_t evt_type = evt_data[0];

	switch (evt_type) {
#if defined(CONFIG_BT_CLASSIC)
	case BT_HCI_EVT_INQUIRY_RESULT_WITH_RSSI:
	case BT_HCI_EVT_EXTENDED_INQUIRY_RESULT:
		return true;
#endif
	case BT_HCI_EVT_LE_META_EVENT: {
		uint8_t subevt_type = evt_data[sizeof(struct bt_hci_evt_hdr)];

		switch (subevt_type) {
		case BT_HCI_EVT_LE_ADVERTISING_REPORT:
			return true;
		default:
			return false;
		}
	}
	default:
		return false;
	}
}

static struct net_buf *bt_b91_evt_recv(const uint8_t *data, size_t len)
{
	bool discardable;
	struct bt_hci_evt_hdr hdr;
	struct net_buf *buf;
	size_t buf_tailroom;

	if (len < sizeof(hdr)) {
		LOG_ERR("Not enough data for event header");
		return NULL;
	}

	discardable = is_hci_event_discardable(data);

	memcpy(&hdr, data, sizeof(hdr));
	data += sizeof(hdr);
	len -= sizeof(hdr);

	if (len != hdr.len) {
		LOG_ERR("Event payload length mismatch (%zu != %u)", len, hdr.len);
		return NULL;
	}

	buf = bt_buf_get_evt(hdr.evt, discardable, K_NO_WAIT);
	if (!buf) {
		if (discardable) {
			LOG_DBG("Discardable buffer pool full, ignoring event");
		} else {
			LOG_ERR("No available event buffers!");
		}
		return NULL;
	}

	net_buf_add_mem(buf, &hdr, sizeof(hdr));

	buf_tailroom = net_buf_tailroom(buf);
	if (buf_tailroom < len) {
		LOG_ERR("Not enough space in buffer %zu/%zu", len, buf_tailroom);
		net_buf_unref(buf);
		return NULL;
	}

	net_buf_add_mem(buf, data, len);
	return buf;
}

static struct net_buf *bt_b91_acl_recv(const uint8_t *data, size_t len)
{
	struct bt_hci_acl_hdr hdr;
	struct net_buf *buf;
	size_t buf_tailroom;

	if (len < sizeof(hdr)) {
		LOG_ERR("Not enough data for ACL header");
		return NULL;
	}

	buf = bt_buf_get_rx(BT_BUF_ACL_IN, K_NO_WAIT);
	if (!buf) {
		LOG_ERR("No available ACL buffers!");
		return NULL;
	}

	memcpy(&hdr, data, sizeof(hdr));
	data += sizeof(hdr);
	len -= sizeof(hdr);

	if (len != sys_le16_to_cpu(hdr.len)) {
		LOG_ERR("ACL payload length mismatch");
		net_buf_unref(buf);
		return NULL;
	}

	net_buf_add_mem(buf, &hdr, sizeof(hdr));

	buf_tailroom = net_buf_tailroom(buf);
	if (buf_tailroom < len) {
		LOG_ERR("Not enough space in buffer %zu/%zu", len, buf_tailroom);
		net_buf_unref(buf);
		return NULL;
	}

	net_buf_add_mem(buf, data, len);
	return buf;
}

/*
 * Callback from b91_bt shim -- called from the controller thread
 * when there is HCI data for the host.
 *
 * Data format: H4 wire format starting from the type byte.
 *   data[0] = H4 type (0x02=ACL, 0x04=Event)
 *   data[1..] = HCI packet
 */
static void hci_b91_host_recv(uint8_t *data, uint16_t len)
{
	const struct device *dev = DEVICE_DT_GET(DT_DRV_INST(0));
	struct hci_b91_data *hci = dev->data;
	struct net_buf *buf;
	uint8_t pkt_type;

	if (!hci->recv || len < 1) {
		return;
	}

	pkt_type = data[0];
	data++;
	len--;

	LOG_HEXDUMP_DBG(data, len, "host recv:");

	switch (pkt_type) {
	case 0x04: /* HCI Event */
		buf = bt_b91_evt_recv(data, len);
		break;
	case 0x02: /* HCI ACL Data */
		buf = bt_b91_acl_recv(data, len);
		break;
	default:
		LOG_ERR("Unknown HCI type 0x%02x", pkt_type);
		return;
	}

	if (buf) {
		hci->recv(dev, buf);
	}
}

/* --------------------------------------------------------------------------
 * BT HCI driver API
 * -------------------------------------------------------------------------- */

static const b91_bt_host_callback_t hci_b91_callbacks = {
	.host_send_available = NULL,
	.host_read_packet = hci_b91_host_recv,
};

static int hci_b91_open(const struct device *dev, bt_hci_recv_t recv)
{
	struct hci_b91_data *data = dev->data;
	int err;

	data->recv = recv;

	/* Register our receive callback before init so we catch early events */
	b91_bt_host_callback_register(&hci_b91_callbacks);

	/* Init BLE controller (also starts controller thread and IRQs) */
	err = b91_bt_controller_init();
	if (err) {
		LOG_ERR("BLE controller init failed: %d", err);
		return err;
	}

	LOG_DBG("B91 BLE HCI opened");
	return 0;
}

static int hci_b91_send(const struct device *dev, struct net_buf *buf)
{
	uint8_t type;

	ARG_UNUSED(dev);

	LOG_DBG("buf %p type %u len %u", buf, bt_buf_get_type(buf), buf->len);

	switch (bt_buf_get_type(buf)) {
	case BT_BUF_CMD:
		type = 0x01; /* HCI Command */
		break;
	case BT_BUF_ACL_OUT:
		type = 0x02; /* HCI ACL Data */
		break;
	default:
		LOG_ERR("Unknown buf type %u", bt_buf_get_type(buf));
		net_buf_unref(buf);
		return -EINVAL;
	}

	LOG_HEXDUMP_DBG(buf->data, buf->len, "HCI TX:");

	b91_bt_host_send_packet(type, buf->data, buf->len);

	net_buf_unref(buf);
	return 0;
}

static int hci_b91_close(const struct device *dev)
{
	ARG_UNUSED(dev);
	LOG_DBG("B91 BLE HCI closed");
	return 0;
}

/* --------------------------------------------------------------------------
 * Device instantiation
 * -------------------------------------------------------------------------- */

static DEVICE_API(bt_hci, hci_b91_api) = {
	.open  = hci_b91_open,
	.send  = hci_b91_send,
	.close = hci_b91_close,
};

#define HCI_B91_INIT(inst)                                                     \
	static struct hci_b91_data hci_b91_data_##inst;                        \
	DEVICE_DT_INST_DEFINE(inst, NULL, NULL,                                \
			      &hci_b91_data_##inst, NULL,                      \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE, \
			      &hci_b91_api);

DT_INST_FOREACH_STATUS_OKAY(HCI_B91_INIT)
