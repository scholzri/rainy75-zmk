#define DT_DRV_COMPAT rainy_behavior_rgb

#include <errno.h>
#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include "../rainy_rgb/engine.h"
#include <dt-bindings/rainy_rgb.h>

LOG_MODULE_REGISTER(behavior_rainy_rgb, CONFIG_LOG_DEFAULT_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

static int on_rgb_pressed(struct zmk_behavior_binding *binding,
                          struct zmk_behavior_binding_event event) {
    switch (binding->param1) {
    case RGB_TOG: rrgb_toggle(); break;
    case RGB_EFF: rrgb_next_effect(); break;
    case RGB_HUI: rrgb_hue_step(+1); break;
    case RGB_HUD: rrgb_hue_step(-1); break;
    case RGB_BRI: rrgb_val_step(+1); break;
    case RGB_BRD: rrgb_val_step(-1); break;
    case RGB_SPI: rrgb_speed_step(+1); break;
    case RGB_SPD: rrgb_speed_step(-1); break;
    case RGB_BAT: rrgb_battery_gauge_show(); break;
    default: return -ENOTSUP;
    }
    return ZMK_BEHAVIOR_OPAQUE;
}

static int on_rgb_released(struct zmk_behavior_binding *binding,
                           struct zmk_behavior_binding_event event) {
    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_rainy_rgb_api = {
    .binding_pressed = on_rgb_pressed,
    .binding_released = on_rgb_released,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL,
                        CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_rainy_rgb_api);

#endif
