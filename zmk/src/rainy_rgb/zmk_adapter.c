#include <string.h>
#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/sys_io.h>
#include <zephyr/init.h>
#include "zmk_adapter.h"
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/hid_indicators.h>
#include <zmk/battery.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/hid_indicators_changed.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/behavior.h>
#include <zmk/matrix.h>
#if IS_ENABLED(CONFIG_ZMK_USB)
#include <zmk/usb.h>
#include <zmk/events/usb_conn_state_changed.h>
#endif
#include "engine.h"
#include "overlay.h"

#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zephyr/bluetooth/conn.h>
#include <zephyr/settings/settings.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/ble_auth_state_changed.h>
#if CONFIG_RAINY75_BLE_OPEN_PROFILE_TIMEOUT > 0
#include <rainy75/events/ble_open_profile_timeout.h>
#endif
#include "ble_status.h"
#endif

LOG_MODULE_REGISTER(rrgb_adapter, CONFIG_LOG_DEFAULT_LEVEL);

#define STRIP_NODE  DT_CHOSEN(zmk_underglow)
#define STRIP_N     DT_PROP(STRIP_NODE, chain_length)

static const struct device *strip = DEVICE_DT_GET(STRIP_NODE);
static struct led_rgb hw[STRIP_N];

int rrgb_strip_init(void) {
    if (!device_is_ready(strip)) {
        LOG_ERR("led_strip not ready");
        return -1;
    }
    if (led_strip_length(strip) < STRIP_N) {
        LOG_ERR("strip too short: %zu < %d", led_strip_length(strip), STRIP_N);
        return -1;
    }
    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_BLE)
/* --- BLE slot status (ble_status.c) ---------------------------------------
 * Every ble_status slot/output/event setter runs in one work item on the
 * system workqueue (single writer). Triggers only schedule it: ZMK's profile
 * and endpoint events, the auth events from patch 0006 and the open profile
 * timeout event of our module (both queued with their payload), the
 * Bluetooth connection callbacks (a background slot of a
 * multilink setup connects or drops without a ZMK event), key releases (the
 * output toggle and a re-select of the active slot raise no event when the
 * effective endpoint does not change) and the settings commit at boot (the
 * profiles are loaded then). Each run polls slots 0..2 again; an unchanged
 * poll is a no-op in ble_status. */
#define RRGB_BLE_SETTLE_MS 50   /* after conn callbacks / key releases */

/* A slot event waiting for the work item, in ble_status terms. */
struct rrgb_ble_qev {
    const char *what;   /* for the log */
    uint8_t ev;         /* enum rrgb_ble_ev */
    uint8_t slot;
    uint8_t digits;
};
K_MSGQ_DEFINE(rrgb_ble_evq, sizeof(struct rrgb_ble_qev), 8, 4);

static void rrgb_ble_queue(const char *what, enum rrgb_ble_ev ev, uint8_t slot, uint8_t digits) {
    struct rrgb_ble_qev q = { .what = what, .ev = ev, .slot = slot, .digits = digits };

    if (k_msgq_put(&rrgb_ble_evq, &q, K_NO_WAIT) != 0) {
        LOG_WRN("ble leds: dropped %s slot %u", what, slot);
    }
}

static void rrgb_ble_auth(const struct zmk_ble_auth_state_changed *a) {
    switch (a->state) {
    case ZMK_BLE_AUTH_PASSKEY_REQ:
        rrgb_ble_queue("passkey req", RRGB_BLE_EV_PASSKEY_REQ, a->profile, a->digits); break;
    case ZMK_BLE_AUTH_PASSKEY_DIGITS:
        rrgb_ble_queue("digits", RRGB_BLE_EV_PASSKEY_DIGITS, a->profile, a->digits); break;
    case ZMK_BLE_AUTH_PAIRED_OK:
        rrgb_ble_queue("paired", RRGB_BLE_EV_PAIRED_OK, a->profile, a->digits); break;
    case ZMK_BLE_AUTH_FAILED:
        rrgb_ble_queue("failed", RRGB_BLE_EV_FAILED, a->profile, a->digits); break;
    case ZMK_BLE_AUTH_CLEARED:
        rrgb_ble_queue("cleared", RRGB_BLE_EV_CLEARED, a->profile, a->digits); break;
    case ZMK_BLE_AUTH_PASSKEY_SUBMITTED:
        rrgb_ble_queue("submitted", RRGB_BLE_EV_PASSKEY_SUBMITTED, a->profile, a->digits); break;
    default:
        break;
    }
}

static void rrgb_ble_refresh(struct k_work *work) {
    static uint8_t last_st[RRGB_BLE_SLOTS] = {0xFF, 0xFF, 0xFF};
    static uint8_t last_active = 0xFE;
    static int last_out = -1;
    struct rrgb_ble_qev q;
    uint32_t tick = rrgb_now();
    ARG_UNUSED(work);

    while (k_msgq_get(&rrgb_ble_evq, &q, K_NO_WAIT) == 0) {
        LOG_INF("ble leds: %s slot %u digits %u @%u", q.what, q.slot, q.digits, tick);
        rrgb_ble_event((enum rrgb_ble_ev)q.ev, q.slot, q.digits, tick);
    }

    uint8_t st[RRGB_BLE_SLOTS];
    for (uint8_t i = 0; i < RRGB_BLE_SLOTS; i++) {
        if (i >= ZMK_BLE_PROFILE_COUNT || zmk_ble_profile_is_open(i)) {
            st[i] = RRGB_BLE_EMPTY;
        } else {
            st[i] = zmk_ble_profile_is_connected(i) ? RRGB_BLE_CONNECTED : RRGB_BLE_PAIRED;
        }
    }
    int idx = zmk_ble_active_profile_index();
    uint8_t active = (idx >= 0 && idx < RRGB_BLE_SLOTS) ? (uint8_t)idx : RRGB_BLE_NONE;
    /* BLE output: the user chose BLE, or the board types over BLE anyway
     * (USB preferred but not ready). Preferred BLE with the slot still
     * connecting falls back to USB in ZMK; it still counts as BLE here, so
     * the connecting animation shows. */
    bool out_ble = zmk_endpoint_get_preferred_transport() == ZMK_TRANSPORT_BLE ||
                   zmk_endpoint_get_selected().transport == ZMK_TRANSPORT_BLE;

    if (memcmp(st, last_st, sizeof(st)) != 0 || active != last_active ||
        (int)out_ble != last_out) {
        LOG_INF("ble leds: slots %u/%u/%u active %d out %s @%u", st[0], st[1], st[2],
                active == RRGB_BLE_NONE ? -1 : active, out_ble ? "ble" : "usb", tick);
        memcpy(last_st, st, sizeof(st));
        last_active = active;
        last_out = out_ble;
    }
    rrgb_ble_set_output_ble(out_ble);
    rrgb_ble_set_slots(st, active, tick);
}
static K_WORK_DELAYABLE_DEFINE(rrgb_ble_work, rrgb_ble_refresh);

static void rrgb_ble_kick(void) { k_work_reschedule(&rrgb_ble_work, K_NO_WAIT); }
/* Deferred trigger: never postpones an already scheduled run. */
static void rrgb_ble_kick_later(void) { k_work_schedule(&rrgb_ble_work, K_MSEC(RRGB_BLE_SETTLE_MS)); }

static void rrgb_ble_conn_changed(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(conn); ARG_UNUSED(reason);
    rrgb_ble_kick_later();
}
BT_CONN_CB_DEFINE(rrgb_ble_conn_cb) = {
    .connected = rrgb_ble_conn_changed,
    .disconnected = rrgb_ble_conn_changed,
};

static int rrgb_ble_settings_commit(void) {
    rrgb_ble_kick();   /* profiles and the preferred transport are loaded */
    return 0;
}
SETTINGS_STATIC_HANDLER_DEFINE(rrgb_ble, "rrgb_ble", NULL, NULL, rrgb_ble_settings_commit, NULL);

static int rrgb_ble_listener(const zmk_event_t *eh) {
    const struct zmk_ble_auth_state_changed *a = as_zmk_ble_auth_state_changed(eh);
    if (a) { rrgb_ble_auth(a); }
#if CONFIG_RAINY75_BLE_OPEN_PROFILE_TIMEOUT > 0
    /* The open slot flashes red like FAILED. The event comes right before
     * the profile change, so the run then sees the returning slot as the
     * new active one and shows its switch confirm. */
    const struct rainy75_ble_open_profile_timeout *t = as_rainy75_ble_open_profile_timeout(eh);
    if (t) { rrgb_ble_queue("open slot timeout", RRGB_BLE_EV_FAILED, t->profile, 0); }
#endif
    rrgb_ble_kick();
    return ZMK_EV_EVENT_BUBBLE;
}
ZMK_LISTENER(rrgb_ble_listener, rrgb_ble_listener);
ZMK_SUBSCRIPTION(rrgb_ble_listener, zmk_ble_active_profile_changed);
ZMK_SUBSCRIPTION(rrgb_ble_listener, zmk_endpoint_changed);
ZMK_SUBSCRIPTION(rrgb_ble_listener, zmk_ble_auth_state_changed);
#if CONFIG_RAINY75_BLE_OPEN_PROFILE_TIMEOUT > 0
ZMK_SUBSCRIPTION(rrgb_ble_listener, rainy75_ble_open_profile_timeout);
#endif
#endif /* CONFIG_ZMK_BLE */

/* Resolve the BLE status key table before any event can arrive (the ZMK
 * events start at APPLICATION level). */
static int rrgb_overlay_early_init(void) {
    rrgb_overlay_init(IS_ENABLED(CONFIG_ZMK_BLE));
    return 0;
}
SYS_INIT(rrgb_overlay_early_init, POST_KERNEL, 99);

/* --- Fn-highlight keys from the live keymap ------------------------------
 * The keys lit while the Fn layer (layer id 1, as tested below) is held are
 * the positions whose layer-1 binding is not &trans. The set is rebuilt from
 * ZMK's keymap each time the layer becomes active, so it follows every change
 * ZMK Studio makes (binding edits, also unsaved ones, save, discard and its
 * "restore stock settings", which raises no event) and costs nothing while
 * the layer is not held. Runs in the layer listener (system workqueue, the
 * key event path): 83 binding lookups. With one physical layout the binding
 * index is the keymap position (physical_layouts.c identity map). */
#define RRGB_FN_LAYER 1

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_behavior_transparent)
#define RRGB_TRANS_NAME DEVICE_DT_NAME(DT_INST(0, zmk_behavior_transparent))
#else
#define RRGB_TRANS_NAME NULL /* no &trans in this build: every bound key lights */
#endif

static const char *rrgb_fn_dev_at(uint16_t pos, void *ctx) {
    const struct zmk_behavior_binding *b =
        zmk_keymap_get_layer_binding_at_idx(RRGB_FN_LAYER, pos);

    ARG_UNUSED(ctx);
    return b != NULL ? b->behavior_dev : NULL;
}

BUILD_ASSERT(ZMK_KEYMAP_LEN <= RRGB_FN_MASK_WORDS * 32,
             "the Fn highlight mask (RRGB_FN_MASK_WORDS) does not cover every keymap position");

static void rrgb_fn_keys_refresh(void) {
    uint32_t mask[RRGB_FN_MASK_WORDS];

    rrgb_fn_mask_build(rrgb_fn_dev_at, NULL, ZMK_KEYMAP_LEN, RRGB_TRANS_NAME, mask);
    rrgb_overlay_set_fn_keys(mask);
}

static int rrgb_event_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev) {
        rrgb_on_key(ev->position, ev->state);
#if IS_ENABLED(CONFIG_ZMK_BLE)
        if (!ev->state) { rrgb_ble_kick_later(); }
#endif
    }

    const struct zmk_layer_state_changed *lev = as_zmk_layer_state_changed(eh);
    if (lev) {
        bool fn = zmk_keymap_layer_active(RRGB_FN_LAYER);

        if (fn) {
            rrgb_fn_keys_refresh();   /* before the first frame that shows it */
        }
        rrgb_overlay_set_fn(fn);
    }

    const struct zmk_hid_indicators_changed *iev = as_zmk_hid_indicators_changed(eh);
    if (iev) { rrgb_overlay_set_caps((iev->indicators & BIT(1)) != 0); }

    const struct zmk_battery_state_changed *bev = as_zmk_battery_state_changed(eh);
    if (bev) { rrgb_overlay_set_battery(bev->state_of_charge); }

#if IS_ENABLED(CONFIG_ZMK_USB)
    /* rgb.val_battery and ind.bat_low apply while no USB host is connected.
     * zmk_usb_is_hid_ready(): a host configured the keyboard, also while it
     * suspends the bus (PC asleep); a cable pull goes through a bus reset,
     * which clears it (zmk usb.c). The event's conn_state alone would count
     * an unconfigured SUSPEND as a host. */
    if (as_zmk_usb_conn_state_changed(eh)) { rrgb_set_usb_host(zmk_usb_is_hid_ready()); }
#endif

    return ZMK_EV_EVENT_BUBBLE;   /* passive observer */
}
ZMK_LISTENER(rrgb_listener, rrgb_event_listener);
ZMK_SUBSCRIPTION(rrgb_listener, zmk_position_state_changed);
ZMK_SUBSCRIPTION(rrgb_listener, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(rrgb_listener, zmk_hid_indicators_changed);
ZMK_SUBSCRIPTION(rrgb_listener, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_USB)
ZMK_SUBSCRIPTION(rrgb_listener, zmk_usb_conn_state_changed);
#endif

static int rrgb_overlay_seed(void) {
    rrgb_overlay_set_caps((zmk_hid_indicators_get_current_profile() & BIT(1)) != 0);
    rrgb_overlay_set_fn(zmk_keymap_layer_active(RRGB_FN_LAYER));
    rrgb_overlay_set_battery(zmk_battery_state_of_charge());
#if IS_ENABLED(CONFIG_ZMK_USB)
    rrgb_set_usb_host(zmk_usb_is_hid_ready());
#endif
    return 0;
}
SYS_INIT(rrgb_overlay_seed, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

void rrgb_strip_show(const struct rrgb *px, uint16_t n) {
    if (n > STRIP_N) { n = STRIP_N; }
    for (uint16_t i = 0; i < n; i++) {
        hw[i] = (struct led_rgb){ .r = px[i].r, .g = px[i].g, .b = px[i].b };
    }
    (void)led_strip_update_rgb(strip, hw, n);
}

/* PC2 gates LED VCC through a MOSFET (active-high). The led_strip driver
 * configures the pin and powers the rail at init, poweroff.c drops it for
 * deep sleep; here the render loop cuts it while the strip stays dark —
 * a blanked WS2812 still draws ~0.5-1 mA quiescent, ~40-80 mA for 83 LEDs.
 *
 * This file owns the pin: everything that drives or samples the rail goes
 * through rrgb_strip_power() / rrgb_strip_rail_state() so the register
 * addresses live in exactly one place. */
#define B91_GPIO_PC_OEN  0x80140312UL   /* output enable, 0 = enabled */
#define B91_GPIO_PC_OUT  0x80140313UL   /* output data */
#define B91_GPIO_PC_GPIO 0x80140316UL   /* GPIO mode enable */

void rrgb_strip_power(bool on) {
#if IS_ENABLED(CONFIG_LED_STRIP_B91_SPI_PC2_POWER)
    if (on) {
        /* Re-assert the whole drive configuration, not just the level.
         * Writing the data bit alone only powers the rail if the pin is
         * still a GPIO output; if anything reconfigured PC2 behind our
         * back, the strip stays dark and the write looks like it worked.
         * (The analog input-buffer and pull settings from the driver's
         * init do not gate the MOSFET, so they are not replayed here.) */
        sys_write8(sys_read8(B91_GPIO_PC_GPIO) | BIT(2), B91_GPIO_PC_GPIO);
        sys_write8(sys_read8(B91_GPIO_PC_OEN) & ~BIT(2), B91_GPIO_PC_OEN);
        sys_write8(sys_read8(B91_GPIO_PC_OUT) | BIT(2), B91_GPIO_PC_OUT);
    } else {
        sys_write8(sys_read8(B91_GPIO_PC_OUT) & ~BIT(2), B91_GPIO_PC_OUT);
    }
    LOG_INF("led rail %s", on ? "on" : "off");
#else
    ARG_UNUSED(on);
#endif
}

uint8_t rrgb_strip_rail_state(void) {
#if IS_ENABLED(CONFIG_LED_STRIP_B91_SPI_PC2_POWER)
    uint8_t state = 0;

    if (sys_read8(B91_GPIO_PC_OUT) & BIT(2)) {
        state |= RRGB_RAIL_HIGH;
    }
    if (!(sys_read8(B91_GPIO_PC_OEN) & BIT(2))) {   /* 0 = output enabled */
        state |= RRGB_RAIL_OUT_EN;
    }
    if (sys_read8(B91_GPIO_PC_GPIO) & BIT(2)) {
        state |= RRGB_RAIL_GPIO_MODE;
    }
    return state;
#else
    return 0;
#endif
}
