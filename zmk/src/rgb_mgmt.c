/*
 * Custom mcumgr group for host-controlled per-key RGB.
 *
 * Lets a host script drive the rainy_rgb engine's direct-pixel mode over the
 * same SMP transport as firmware updates (USB CDC-ACM serial; BLE transport
 * works too if enabled).
 *
 * Group ID 65 (MGMT_GROUP_ID_PERUSER + 1), 4 commands:
 *   0: set    {"px": bstr}                 -> {"rc": int}
 *             px = N quads of [keymap_position, r, g, b]. First set after
 *             normal mode starts from an all-black frame.
 *   1: fill   {"r": uint, "g": uint, "b": uint} -> {"rc": int}
 *   2: clear  {}                            -> {"rc": int}  (back to effects)
 *   3: info   (read)                        -> {"rc": 0, "n": 83, "host": bool}
 *
 * Positions are keymap positions (0..82, ISO row-major) — the engine's
 * led_map translates to physical LED indices, so the same host code works
 * on ISO and ANSI boards. Host mode is not persisted; any physical Fn+RGB
 * control exits it (escape hatch in engine.c).
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/kernel.h>
#include <zephyr/mgmt/mcumgr/mgmt/mgmt.h>
#include <zephyr/mgmt/mcumgr/mgmt/handlers.h>
#include <zephyr/mgmt/mcumgr/smp/smp.h>
#include <zcbor_common.h>
#include <zcbor_decode.h>
#include <zcbor_encode.h>
#include <mgmt/mcumgr/util/zcbor_bulk.h>
#include <zephyr/logging/log.h>

#include "rainy_rgb/engine.h"

LOG_MODULE_REGISTER(rgb_mgmt, LOG_LEVEL_INF);

#define RGB_MGMT_GROUP_ID   (MGMT_GROUP_ID_PERUSER + 1)

#define RGB_MGMT_ID_SET     0
#define RGB_MGMT_ID_FILL    1
#define RGB_MGMT_ID_CLEAR   2
#define RGB_MGMT_ID_INFO    3

#define RGB_MGMT_POSITIONS  83
/* 4 bytes per pixel quad; full board in one request still fits the SMP
 * netbuf, but cap defensively. */
#define RGB_MGMT_MAX_QUADS  RGB_MGMT_POSITIONS

static int rgb_mgmt_set(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	struct zcbor_string px = {0};
	size_t decoded;

	struct zcbor_map_decode_key_val decode_map[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("px", zcbor_bstr_decode, &px),
	};

	if (zcbor_map_decode_bulk(zsd, decode_map, ARRAY_SIZE(decode_map),
				  &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	if (px.len == 0 || (px.len % 4) != 0 ||
	    (px.len / 4) > RGB_MGMT_MAX_QUADS) {
		LOG_ERR("set: bad px len %zu", px.len);
		return MGMT_ERR_EINVAL;
	}

	rrgb_host_set_pixels(px.value, px.len / 4);

	bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int rgb_mgmt_fill(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;
	zcbor_state_t *zsd = ctxt->reader->zs;
	uint32_t r = 0, g = 0, b = 0;
	size_t decoded;

	struct zcbor_map_decode_key_val decode_map[] = {
		ZCBOR_MAP_DECODE_KEY_DECODER("r", zcbor_uint32_decode, &r),
		ZCBOR_MAP_DECODE_KEY_DECODER("g", zcbor_uint32_decode, &g),
		ZCBOR_MAP_DECODE_KEY_DECODER("b", zcbor_uint32_decode, &b),
	};

	if (zcbor_map_decode_bulk(zsd, decode_map, ARRAY_SIZE(decode_map),
				  &decoded) != 0) {
		return MGMT_ERR_EINVAL;
	}

	if (r > 255 || g > 255 || b > 255) {
		return MGMT_ERR_EINVAL;
	}

	rrgb_host_fill(r, g, b);

	bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int rgb_mgmt_clear(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	rrgb_host_clear();

	bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0);
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static int rgb_mgmt_info(struct smp_streamer *ctxt)
{
	zcbor_state_t *zse = ctxt->writer->zs;

	bool ok = zcbor_tstr_put_lit(zse, "rc") && zcbor_int32_put(zse, 0) &&
		  zcbor_tstr_put_lit(zse, "n") &&
		  zcbor_uint32_put(zse, RGB_MGMT_POSITIONS) &&
		  zcbor_tstr_put_lit(zse, "host") &&
		  zcbor_bool_put(zse, rrgb_host_active());
	return ok ? MGMT_ERR_EOK : MGMT_ERR_EMSGSIZE;
}

static const struct mgmt_handler rgb_mgmt_handlers[] = {
	[RGB_MGMT_ID_SET]   = { .mh_read = NULL, .mh_write = rgb_mgmt_set },
	[RGB_MGMT_ID_FILL]  = { .mh_read = NULL, .mh_write = rgb_mgmt_fill },
	[RGB_MGMT_ID_CLEAR] = { .mh_read = NULL, .mh_write = rgb_mgmt_clear },
	[RGB_MGMT_ID_INFO]  = { .mh_read = rgb_mgmt_info, .mh_write = NULL },
};

static struct mgmt_group rgb_mgmt_group = {
	.mg_handlers = rgb_mgmt_handlers,
	.mg_handlers_count = ARRAY_SIZE(rgb_mgmt_handlers),
	.mg_group_id = RGB_MGMT_GROUP_ID,
};

static void rgb_mgmt_register(void)
{
	mgmt_register_group(&rgb_mgmt_group);
	LOG_INF("rgb_mgmt registered (group %d)", RGB_MGMT_GROUP_ID);
}

MCUMGR_HANDLER_DEFINE(rgb_mgmt, rgb_mgmt_register);
