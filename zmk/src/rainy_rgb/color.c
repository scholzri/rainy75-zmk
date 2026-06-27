#include "color.h"

uint8_t scale8(uint8_t v, uint8_t scale) {
    /* "+1" form so scale8(x,255) == x exactly (FastLED SCALE8_FIXED). */
    return (uint8_t)(((uint16_t)v * (uint16_t)(scale + 1)) >> 8);
}

/* Quadratic sine approximation, 256 phase, output centered at 128. */
uint8_t sin8(uint8_t theta) {
    uint8_t offset = theta;
    if (theta & 0x40) { offset = 255 - offset; }
    offset &= 0x3F;                 /* 0..63 within quarter */
    uint16_t y = (uint16_t)offset * offset;   /* 0..3969 */
    y = (y * 2) >> 6;               /* scale into ~0..124 */
    uint8_t v = (uint8_t)y;
    if (theta & 0x80) { return (uint8_t)(128 - v); }
    return (uint8_t)(128 + v);
}

uint8_t hypot8(int dx, int dy) {
    int ax = dx < 0 ? -dx : dx;
    int ay = dy < 0 ? -dy : dy;
    int hi = ax > ay ? ax : ay;
    int lo = ax > ay ? ay : ax;
    int d = hi + (lo >> 1);
    return (uint8_t)(d > 255 ? 255 : d);
}

/* Standard 6-sector HSV. */
struct rrgb hsv2rgb(uint8_t h, uint8_t s, uint8_t v) {
    struct rrgb out;
    if (s == 0) { out.r = out.g = out.b = v; return out; }
    uint8_t region = h / 43;            /* 0..5 */
    uint8_t rem = (h - region * 43) * 6;/* 0..255 within sector */
    uint8_t p = scale8(v, 255 - s);
    uint8_t q = scale8(v, 255 - scale8(s, rem));
    uint8_t t = scale8(v, 255 - scale8(s, 255 - rem));
    switch (region) {
    case 0:  out.r = v; out.g = t; out.b = p; break;
    case 1:  out.r = q; out.g = v; out.b = p; break;
    case 2:  out.r = p; out.g = v; out.b = t; break;
    case 3:  out.r = p; out.g = q; out.b = v; break;
    case 4:  out.r = t; out.g = p; out.b = v; break;
    default: out.r = v; out.g = p; out.b = q; break;
    }
    return out;
}
