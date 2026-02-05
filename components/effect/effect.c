#include "effect.h"

#include <stdlib.h>
#include <string.h>

static int16_t *delay_line = NULL;
static int delay_samples = 0;
static int delay_ptr = 0;

void effector_init(uint32_t sample_rate) {
    delay_samples = (sample_rate * 300) / 1000; // 300ms 지연 버퍼 할당
    delay_line = (int16_t *)calloc(delay_samples, sizeof(int16_t)); // malloc과 비슷하지만 메모리를 할당후 0으로 초기화
}

void effector_apply(int16_t *buffer, int samples, effect_mode_t mode) {
    if (mode == FX_NONE) return;

    for (int i = 0; i < samples; i++) {
        int32_t sample = buffer[i];

        switch (mode) {
            case FX_OVERDRIVE:
                if (sample > 8000) sample = 8000 + (sample - 8000) / 4;
                else if (sample < -8000) sample = -8000 + (sample + 8000) / 4;
                buffer[i] = (int16_t)sample;
                break;

            case FX_DISTORTION:
                sample *= 4;
                if (sample > 12000) sample = 12000;
                else if (sample < -12000) sample = -12000;
                buffer[i] = (int16_t)sample;
                break;

            case FX_DELAY:
                if (delay_line) {
                    int32_t delayed = delay_line[delay_ptr];
                    int32_t mixed = sample + (delayed * 5 / 10); // 50% 피드백

                    // Clipping 보호
                    if (mixed > 32767) mixed = 32767;
                    if (mixed < -32768) mixed = -32768;

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