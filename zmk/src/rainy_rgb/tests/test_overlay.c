#include "../overlay.h"
#include "../led_map.h"
#include "test.h"
#include <string.h>

int main(void) {
    struct rrgb px[83];

    /* reset state */
    rrgb_overlay_set_caps(false);
    rrgb_overlay_set_fn(false);
    rrgb_overlay_set_battery(0);

    /* nothing active -> render leaves pixels untouched */
    memset(px, 7, sizeof(px));
    rrgb_overlay_render(px, 83, 100);
    CHECK(px[0].r == 7 && px[10].g == 7);           /* untouched */

    /* CapsLock on -> CapsLock LED (pos 44) is white, others untouched */
    memset(px, 0, sizeof(px));
    rrgb_overlay_set_caps(true);
    rrgb_overlay_render(px, 83, 100);
    int caps_led = rrgb_led_for_position(44);
    CHECK(caps_led >= 0);
    CHECK(px[caps_led].r == 255 && px[caps_led].g == 255 && px[caps_led].b == 255);
    rrgb_overlay_set_caps(false);

    /* Fn active -> only Fn keys lit white, rest black */
    for (int i = 0; i < 83; i++) { px[i] = (struct rrgb){50,50,50}; }
    rrgb_overlay_set_fn(true);
    rrgb_overlay_render(px, 83, 100);
    int led_esc = rrgb_led_for_position(0);    /* ESC: Fn-active */
    int led_q   = rrgb_led_for_position(31);   /* Q: NOT Fn-active */
    CHECK(px[led_esc].r == 255);               /* Fn key lit */
    CHECK((px[led_q].r|px[led_q].g|px[led_q].b) == 0);  /* non-Fn key black */
    rrgb_overlay_set_fn(false);

    /* battery gauge: 50% -> 5 of 10 segments lit on number row (pos 16..25) */
    memset(px, 0, sizeof(px));
    rrgb_overlay_set_battery(50);
    rrgb_overlay_battery_show(0);              /* window until tick 90 */
    rrgb_overlay_render(px, 83, 10);           /* tick 10 < 90 -> active */
    int seg0 = rrgb_led_for_position(16);      /* first segment (lit) */
    int seg9 = rrgb_led_for_position(25);      /* last segment (unlit, dim) */
    CHECK((px[seg0].r|px[seg0].g|px[seg0].b) > 30);   /* lit */
    CHECK(px[seg9].r < 20 && px[seg9].g < 20 && px[seg9].b < 20); /* dim/unlit */

    /* gauge window expires */
    memset(px, 0, sizeof(px));
    rrgb_overlay_render(px, 83, 100);          /* tick 100 >= 90 -> no gauge */
    CHECK((px[seg0].r|px[seg0].g|px[seg0].b) == 0);

    /* rrgb_overlay_active: true while caps/fn/battery-window, else false
       (so indicators render even when decorative RGB is off) */
    rrgb_overlay_set_caps(false);
    rrgb_overlay_set_fn(false);
    rrgb_overlay_battery_show(0);              /* window until tick 90 */
    CHECK(rrgb_overlay_active(10));            /* battery window open */
    CHECK(!rrgb_overlay_active(100));          /* window closed, nothing else */
    rrgb_overlay_set_caps(true);
    CHECK(rrgb_overlay_active(100));           /* caps */
    rrgb_overlay_set_caps(false);
    rrgb_overlay_set_fn(true);
    CHECK(rrgb_overlay_active(100));           /* fn */
    rrgb_overlay_set_fn(false);
    CHECK(!rrgb_overlay_active(100));          /* nothing active */

    DONE();
}
