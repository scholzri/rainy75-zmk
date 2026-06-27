#ifndef RAINY_RGB_COLOR_H
#define RAINY_RGB_COLOR_H
#include <stdint.h>

struct rrgb { uint8_t r, g, b; };

/* 8-bit HSV -> RGB (FastLED "rainbow" style, all args 0..255). */
struct rrgb hsv2rgb(uint8_t h, uint8_t s, uint8_t v);

/* 8-bit sine: theta 0..255 -> 0..255, sin8(0)=128, sin8(64)≈252. */
uint8_t sin8(uint8_t theta);

/* scale a byte by another byte treated as a 0..1 fraction (x*scale/255). */
uint8_t scale8(uint8_t v, uint8_t scale);

/* Approximate Euclidean distance |(dx,dy)|, clamped to 0..255.
 * alpha-max-beta-min: max + min/2 (~6% over on the diagonal). */
uint8_t hypot8(int dx, int dy);

#endif
