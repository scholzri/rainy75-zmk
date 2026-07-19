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
#include <zmk/events/activity_state_changed.h>
#include "engine.h"
#include "overlay.h"

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

static int rrgb_event_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *ev = as_zmk_position_state_changed(eh);
    if (ev) { rrgb_on_key(ev->position, ev->state); }

    const struct zmk_layer_state_changed *lev = as_zmk_layer_state_changed(eh);
    if (lev) { rrgb_overlay_set_fn(zmk_keymap_layer_active(1)); }

    const struct zmk_hid_indicators_changed *iev = as_zmk_hid_indicators_changed(eh);
    if (iev) { rrgb_overlay_set_caps((iev->indicators & BIT(1)) != 0); }

    const struct zmk_battery_state_changed *bev = as_zmk_battery_state_changed(eh);
    if (bev) { rrgb_overlay_set_battery(bev->state_of_charge); }

#if IS_ENABLED(CONFIG_RAINY_RGB_IDLE_BLANK)
    const struct zmk_activity_state_changed *aev = as_zmk_activity_state_changed(eh);
    if (aev) { rrgb_set_idle(aev->state != ZMK_ACTIVITY_ACTIVE); }
#endif

    return ZMK_EV_EVENT_BUBBLE;   /* passive observer */
}
ZMK_LISTENER(rrgb_listener, rrgb_event_listener);
ZMK_SUBSCRIPTION(rrgb_listener, zmk_position_state_changed);
ZMK_SUBSCRIPTION(rrgb_listener, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(rrgb_listener, zmk_hid_indicators_changed);
ZMK_SUBSCRIPTION(rrgb_listener, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_RAINY_RGB_IDLE_BLANK)
ZMK_SUBSCRIPTION(rrgb_listener, zmk_activity_state_changed);
#endif

static int rrgb_overlay_seed(void) {
    rrgb_overlay_set_caps((zmk_hid_indicators_get_current_profile() & BIT(1)) != 0);
    rrgb_overlay_set_fn(zmk_keymap_layer_active(1));
    rrgb_overlay_set_battery(zmk_battery_state_of_charge());
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
 * a blanked WS2812 still draws ~0.5-1 mA quiescent, ~40-80 mA for 83 LEDs. */
#define B91_GPIO_PC_OUT 0x80140313UL

void rrgb_strip_power(bool on) {
#if IS_ENABLED(CONFIG_LED_STRIP_B91_SPI_PC2_POWER)
    if (on) {
        sys_write8(sys_read8(B91_GPIO_PC_OUT) | BIT(2), B91_GPIO_PC_OUT);
    } else {
        sys_write8(sys_read8(B91_GPIO_PC_OUT) & ~BIT(2), B91_GPIO_PC_OUT);
    }
    LOG_INF("led rail %s", on ? "on" : "off");
#else
    ARG_UNUSED(on);
#endif
}
