/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * &os_key LGUI / &os_key LALT: the left GUI and Alt keys as the runtime
 * settings kb.os and kb.gui_lock say. The decisions (Mac swap, GUI lock, the
 * key held per position) are in os_key/os_key.c, host tested; this file
 * hands ZMK's key events to them and raises the keycode they return, as &kp
 * does. config/cfg_table.c pushes the settings (os_key_set_mode); without
 * CONFIG_RAINY75_CONFIG the keys are plain GUI and Alt. It also counts the
 * key positions bound to &os_key for the read-only setting kb.os_keys.
 */

#define DT_DRV_COMPAT rainy_behavior_os_key

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <dt-bindings/zmk/keys.h>
#include <zmk/behavior.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>

#include "os_key/os_key.h"

LOG_MODULE_REGISTER(behavior_os_key, CONFIG_ZMK_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(OS_KEY_LGUI == LGUI && OS_KEY_LALT == LALT, "os_key.h keycodes differ from ZMK's");
BUILD_ASSERT(ZMK_KEYMAP_LEN <= OS_KEY_COUNT_MAX,
             "more key positions than the range of kb.os_keys (OS_KEY_COUNT_MAX)");

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata os_key_values[] = {
    {
        .display_name = "Win (Option on a Mac)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = LGUI,
    },
    {
        .display_name = "Alt (Command on a Mac)",
        .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
        .value = LALT,
    },
};

static const struct behavior_parameter_metadata_set os_key_set = {
    .param1_values = os_key_values,
    .param1_values_len = ARRAY_SIZE(os_key_values),
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = 1,
    .sets = &os_key_set,
};

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static int on_os_key_pressed(struct zmk_behavior_binding *binding,
                             struct zmk_behavior_binding_event event) {
    uint32_t code;

    if (!os_key_param_valid(binding->param1)) {
        LOG_WRN("&os_key 0x%08x: the parameter must be LGUI or LALT", binding->param1);
        return -ENOTSUP;
    }
    code = os_key_press(event.position, binding->param1);
    if (code == 0) {
        return ZMK_BEHAVIOR_OPAQUE; /* GUI lock, or nothing to hold it in */
    }
    return raise_zmk_keycode_state_changed_from_encoded(code, true, event.timestamp);
}

static int on_os_key_released(struct zmk_behavior_binding *binding,
                              struct zmk_behavior_binding_event event) {
    uint32_t code = os_key_release(event.position);

    if (code == 0) {
        return ZMK_BEHAVIOR_OPAQUE;
    }
    return raise_zmk_keycode_state_changed_from_encoded(code, false, event.timestamp);
}

static const struct behavior_driver_api behavior_os_key_api = {
    .binding_pressed = on_os_key_pressed,
    .binding_released = on_os_key_released,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, &behavior_os_key_api);

/* kb.os_keys: the behavior bound at (layer index, position) in the live
 * keymap, which ZMK Studio edits. Layer indexes, not ids: a layer Studio
 * removed has no id and is skipped, as ZMK's own key processing skips it.
 * Called from the mcumgr thread; it reads the bindings without a lock, as
 * ZMK's key processing does (each behavior name pointer is one word). */
static const char *os_key_dev_at(uint8_t layer, uint16_t pos, void *ctx) {
    zmk_keymap_layer_id_t id = zmk_keymap_layer_index_to_id(layer);
    const struct zmk_behavior_binding *b;

    ARG_UNUSED(ctx);
    if (id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        return NULL;
    }
    b = zmk_keymap_get_layer_binding_at_idx(id, pos);
    return b != NULL ? b->behavior_dev : NULL;
}

uint16_t os_key_bound_count(void) {
    return os_key_count_bound(os_key_dev_at, NULL, ZMK_KEYMAP_LAYERS_LEN, ZMK_KEYMAP_LEN,
                              DEVICE_DT_NAME(DT_DRV_INST(0)));
}

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
