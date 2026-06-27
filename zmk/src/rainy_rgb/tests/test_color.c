#include "../color.h"
#include "test.h"

int main(void) {
    /* hue 0 = red, full sat/val */
    struct rrgb red = hsv2rgb(0, 255, 255);
    CHECK(red.r > 250 && red.g < 5 && red.b < 5);
    /* hue ~85 (1/3) = green */
    struct rrgb green = hsv2rgb(85, 255, 255);
    CHECK(green.g > 200 && green.r < 60 && green.b < 60);
    /* hue ~170 (2/3) = blue */
    struct rrgb blue = hsv2rgb(170, 255, 255);
    CHECK(blue.b > 200 && blue.r < 60 && blue.g < 60);
    /* sat 0 = white-ish (r=g=b=v) */
    struct rrgb white = hsv2rgb(100, 0, 200);
    CHECK(white.r == white.g && white.g == white.b && white.r > 180);
    /* val 0 = black */
    struct rrgb black = hsv2rgb(50, 255, 0);
    CHECK(black.r == 0 && black.g == 0 && black.b == 0);
    /* sin8 range + midpoints */
    CHECK(sin8(0) <= 130 && sin8(0) >= 125);     /* ~128 */
    CHECK(sin8(64) >= 250);                        /* peak ~255 */
    CHECK(sin8(192) <= 5);                         /* trough ~0 */
    /* scale8 */
    CHECK(scale8(255, 128) >= 126 && scale8(255, 128) <= 129);
    CHECK(scale8(100, 0) == 0);
    CHECK(scale8(200, 255) == 200);
    /* hypot8: 0 at origin, exact on axes, ~5 for 3-4, monotonic, clamped */
    CHECK(hypot8(0, 0) == 0);
    CHECK(hypot8(10, 0) == 10);
    CHECK(hypot8(0, -10) == 10);
    CHECK(hypot8(3, 4) >= 4 && hypot8(3, 4) <= 7);     /* ~5 (approx) */
    CHECK(hypot8(-3, -4) == hypot8(3, 4));              /* sign-independent */
    CHECK(hypot8(200, 200) == 255);                    /* clamped */
    CHECK(hypot8(20, 0) > hypot8(10, 0));              /* monotonic */
    DONE();
}
