#include "../effects.h"
#include "test.h"
#include <string.h>

int main(void) {
    struct rrgb px[83];
    struct rgb_frame f = { .px = px, .n = 83, .tick = 0,
        .hue = 0, .sat = 255, .val = 255, .speed = 32, .xy = 0, .last_press_tick = 0 };

    /* registry has >= 2 effects and all have non-null render */
    CHECK(rrgb_effect_count >= 2);
    for (uint16_t i = 0; i < rrgb_effect_count; i++) { CHECK(rrgb_effects[i].render != 0); }

    /* solid: all pixels identical, red at hue 0 */
    fx_solid(&f);
    CHECK(px[0].r > 250 && px[0].g < 5);
    CHECK(memcmp(&px[0], &px[82], sizeof(px[0])) == 0);

    /* rainbow wave: pixels vary along the strip (not all identical) */
    fx_rainbow_wave(&f);
    int differ = memcmp(&px[0], &px[41], sizeof(px[0])) != 0;
    CHECK(differ);

    /* rainbow animates off f->phase (FPS-independent), not the frame counter */
    f.phase = 0;  fx_rainbow_wave(&f); struct rrgb rb0 = px[0];
    f.phase = 40; fx_rainbow_wave(&f);
    CHECK(memcmp(&rb0, &px[0], sizeof(rb0)) != 0);
    f.phase = 0;

    /* every effect fills all pixels without reading uninit / crashing,
       and produces at least one lit pixel at full val for several ticks */
    void (*fns[])(struct rgb_frame *) = {
        fx_plasma, fx_twinkle, fx_comet, fx_aurora };
    for (unsigned e = 0; e < sizeof(fns)/sizeof(fns[0]); e++) {
        int any_lit = 0;
        for (uint32_t tk = 0; tk < 60; tk++) {
            f.tick = tk;
            f.phase = tk * 4;          /* advance the shared animation phase */
            fns[e](&f);
            for (int i = 0; i < 83; i++) {
                if (f.px[i].r | f.px[i].g | f.px[i].b) { any_lit = 1; }
            }
        }
        CHECK(any_lit);
    }
    /* reactive: brighter right after a press than long after */
    f.val = 200; f.hue = 0; f.sat = 255;
    f.last_press_tick = 100; f.tick = 100; fx_reactive_pulse(&f);
    uint8_t bright = f.px[0].r;
    f.tick = 130; fx_reactive_pulse(&f);   /* 30 frames later */
    uint8_t dim = f.px[0].r;
    CHECK(bright > dim);

    /* ripple: one active ring. radius = (age * (speed/16+1)*10) >> 4
     * = (16 * 50) >> 4 = 50, so the LED at distance 50 is on the ring;
     * origin (d=0) and far (d=150) are dark. */
    {
        struct led_xy xy3[3] = { {100,50}, {150,50}, {250,50} }; /* d = 0, 50, 150 */
        struct rrgb_ripple rip[1] = { { .x=100, .y=50, .start_tick=0, .active=true } };
        struct rrgb px3[3];
        struct rgb_frame rf = { .px=px3, .n=3, .tick=16, .hue=0, .sat=255,
            .val=255, .speed=64, .xy=xy3, .ripples=rip, .ripple_count=1 };
        fx_ripple(&rf);
        CHECK((px3[0].r|px3[0].g|px3[0].b) == 0);   /* origin inside ring -> dark */
        CHECK((px3[1].r|px3[1].g|px3[1].b) != 0);   /* d=50 on the expanding ring -> lit */
        CHECK((px3[2].r|px3[2].g|px3[2].b) == 0);   /* d=150 far outside -> dark */
    }
    /* ripple NULL-safe */
    { struct rrgb pxn[3]; struct rgb_frame rf = { .px=pxn, .n=3, .xy=0, .ripples=0 };
      fx_ripple(&rf); CHECK((pxn[0].r|pxn[0].g|pxn[0].b)==0); }

    {
        struct led_xy xyf[3] = { {10,10}, {120,50}, {250,90} };
        struct rrgb pxf[3];
        struct rgb_frame wf = { .px=pxf, .n=3, .tick=5, .hue=0, .sat=255, .val=255, .speed=32, .xy=xyf };
        int any=0; for (uint32_t t=0;t<40;t++){ wf.tick=t; wf.phase=t*4; fx_wave(&wf);
            for(int i=0;i<3;i++) any|=pxf[i].r|pxf[i].g|pxf[i].b; }
        CHECK(any);                         /* wave lights pixels */
        /* rain: tile LEDs across x at a low row so some drop column must overlap
         * one (deterministic, independent of the drops' random x positions). */
        struct led_xy xyr[22]; struct rrgb pxr[22];
        for (int i=0;i<22;i++){ xyr[i].x=(uint8_t)(i*12); xyr[i].y=100; }
        struct rgb_frame rnf = { .px=pxr, .n=22, .hue=160, .sat=255, .val=255, .speed=32, .xy=xyr };
        any=0; for (uint32_t t=0;t<120;t++){ rnf.tick=t; fx_rain(&rnf);
            for(int i=0;i<22;i++) any|=pxr[i].r|pxr[i].g|pxr[i].b; }
        CHECK(any);                         /* rain lights pixels over time */
        /* NULL-xy safe */
        struct rgb_frame nf = { .px=pxf, .n=3, .xy=0 };
        fx_wave(&nf); CHECK((pxf[0].r|pxf[0].g|pxf[0].b)==0);
        fx_rain(&nf); CHECK((pxf[0].r|pxf[0].g|pxf[0].b)==0);
    }

    {
        uint8_t heat[3] = { 0, 200, 0 };
        struct rrgb pxh[3];
        struct rgb_frame hf = { .px=pxh, .n=3, .hue=0, .sat=255, .val=255, .key_heat=heat };
        fx_heatmap(&hf);
        CHECK((pxh[0].r|pxh[0].g|pxh[0].b) == 0);   /* cold = black */
        CHECK((pxh[1].r|pxh[1].g|pxh[1].b) != 0);   /* hot = lit */
        struct rgb_frame nf = { .px=pxh, .n=3, .key_heat=0 };
        fx_heatmap(&nf); CHECK((pxh[0].r|pxh[0].g|pxh[0].b)==0);  /* NULL-safe */
    }
    /* registry: 11 effects (fire + calibrate removed) */
    CHECK(rrgb_effect_count == 11);
    CHECK(rrgb_effects[rrgb_effect_count-1].render == fx_heatmap);

    DONE();
}
