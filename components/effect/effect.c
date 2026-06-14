#include "effect.h"

#include <stdlib.h>
#include <string.h>

static int16_t *delay_line = NULL;
static int delay_samples = 0;
static int delay_ptr = 0;

void effector_init(uint32_t sample_rate)
{
    free(delay_line);
    delay_line = NULL;
    delay_ptr = 0;

    delay_samples = (sample_rate * 300) / 1000;
    if (delay_samples <= 0) {
        return;
    }

    delay_line = (int16_t *)calloc(delay_samples, sizeof(int16_t));
}

void effector_apply(int16_t *buffer, int samples, effect_mode_t mode)
{
    if (buffer == NULL || samples <= 0 || mode == FX_NONE || mode == FX_CLEAN) {
        return;
    }

    for (int i = 0; i < samples; i++) {
        int32_t sample = buffer[i];

        switch (mode) {
            case FX_OVERDRIVE:
                if (sample > 8000) {
                    sample = 8000 + (sample - 8000) / 4;
                } else if (sample < -8000) {
                    sample = -8000 + (sample + 8000) / 4;
                }
                buffer[i] = (int16_t)sample;
                break;

            case FX_DISTORTION:
                sample *= 4;
                if (sample > 12000) {
                    sample = 12000;
                } else if (sample < -12000) {
                    sample = -12000;
                }
                buffer[i] = (int16_t)sample;
                break;

            case FX_DELAY:
                if (delay_line && delay_samples > 0) {
                    int32_t delayed = delay_line[delay_ptr];
                    int32_t mixed = sample + (delayed * 5 / 10);

                    if (mixed > 32767) {
                        mixed = 32767;
                    } else if (mixed < -32768) {
                        mixed = -32768;
                    }

                    buffer[i] = (int16_t)mixed;
                    delay_line[delay_ptr] = (int16_t)mixed;
                    delay_ptr = (delay_ptr + 1) % delay_samples;
                }
                break;

            default:
                break;
        }
    }
}
