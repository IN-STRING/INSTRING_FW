#pragma once

#include <stdint.h>

typedef enum {
    FX_NONE = 0,
    FX_OVERDRIVE,
    FX_DISTORTION,
    FX_DELAY,
    FX_CLEAN,
    FX_MODE_MAX
} effect_mode_t;

void effector_init(uint32_t sample_rate);
void effector_apply(int16_t *buffer, int samples, effect_mode_t mode);