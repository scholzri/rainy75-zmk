/*
 * Copyright (c) 2026 scholzri
 * SPDX-License-Identifier: Apache-2.0
 *
 * The runtime settings of this keyboard (cfg_table.c). Owners get the
 * non-proxied values pushed (push() in cfg_table.c) or read them with
 * cfg_u(CFG_...). New settings are only ever appended (stable indexes are
 * not part of the protocol, keys are, but appending keeps diffs small).
 */

#ifndef RAINY75_CFG_TABLE_H
#define RAINY75_CFG_TABLE_H

enum cfg_id {
    CFG_RGB_ON,
    CFG_RGB_EFFECT,
    CFG_RGB_HUE,
    CFG_RGB_SAT,
    CFG_RGB_VAL,
    CFG_RGB_SPEED,
    CFG_RGB_BOOT_EFFECT,
    CFG_RGB_CYCLE,
    CFG_RGB_VAL_BATTERY,
    CFG_RGB_IDLE_S,
    CFG_RGB_IDLE_MODE,
    CFG_IND_CAPS_STYLE,
    CFG_IND_CAPS_COLOR,
    CFG_IND_FN_HIGHLIGHT,
    CFG_IND_PASSKEY_GUIDE,
    CFG_IND_BAT_LOW,
    CFG_KB_OS,
    CFG_KB_GUI_LOCK,
    CFG_KB_OS_KEYS,
    CFG_KB_SLEEP_MIN,
    CFG_KB_SLEEP_ON_USB,
    CFG_ID_COUNT
};

/* Called by the store once the stored settings are loaded at boot. */
void cfg_table_loaded(void);

#endif /* RAINY75_CFG_TABLE_H */
