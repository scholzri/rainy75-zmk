#ifndef RAINY_RGB_ZMK_ADAPTER_H
#define RAINY_RGB_ZMK_ADAPTER_H
#include <stdint.h>
#include "color.h"
int rrgb_strip_init(void);
void rrgb_strip_show(const struct rrgb *px, uint16_t n);
#endif
