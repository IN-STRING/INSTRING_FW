#include "piezo.h"
#include "adc_share.h"
#include "effect.h"

#include <math.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "dsps_fft2r.h"
#include "dsps_wind.h"
#include "rom/ets_sys.h"
#include "esp_log.h"

#define SAMPLES 2048
#define SAMPLING_FREQ 4000
#define ADC_CHAN ADC_CHANNEL_3
#define NOTE_COUNT 6

static float fft_input[SAMPLES * 2];
static float window[SAMPLES];

static const float guitar_notes[NOTE_COUNT] = {82.41f, 110.00f, 146.83f, 196.00f, 246.94f, 329.63f};
const char *note_names[NOTE_COUNT] = {"6E", "5A", "4D", "3G", "2B", "1E"};

volatile float diff = 0.0f;
volatile int note_idx = -1;
extern volatile effect_mode_t current_mode;

static const char *TAG = "PIEZO";

static int find_note(float freq)
{
    int closest = 0;
    float min_diff = fabsf(freq - guitar_notes[0]);

    for (int i = 1; i < NOTE_COUNT; i++) {
        float note_diff = fabsf(freq - guitar_notes[i]);
        if (note_diff < min_diff) {
            min_diff = note_diff;
            closest = i;
        }
    }

    return closest;
}

static void tuning_task(void *pram)
{
    while (1) {
        if (current_mode != FX_TUNER) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for (int i = 0; i < SAMPLES; i++) {
            int raw = 2048;
            if (adc_share_read(ADC_CHAN, &raw) != ESP_OK) {
                raw = 2048;
            }

            fft_input[i * 2] = (float)(raw - 2048) * window[i];
            fft_input[i * 2 + 1] = 0.0f;

            ets_delay_us(1000000 / SAMPLING_FREQ);
        }

        dsps_fft2r_fc32(fft_input, SAMPLES);
        dsps_bit_rev_fc32(fft_input, SAMPLES);
        dsps_cplx2reC_fc32(fft_input, SAMPLES);

        float max_mag = 0.0f;
        int peak_idx = 0;
        for (int i = 10; i < SAMPLES / 2; i++) {
            float real = fft_input[i * 2];
            float imag = fft_input[i * 2 + 1];
            float mag = (real * real) + (imag * imag);

            if (mag > max_mag) {
                max_mag = mag;
                peak_idx = i;
            }
        }

        if (max_mag > 100000.0f) {
            float freq = (float)peak_idx * SAMPLING_FREQ / SAMPLES;
            note_idx = find_note(freq);
            diff = freq - guitar_notes[note_idx];

            ESP_LOGI(TAG, "Note: %s | Freq: %.2f Hz | Diff: %+.2f Hz", note_names[note_idx], freq, diff);
        } else {
            note_idx = -1;
            diff = 0.0f;
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void piezo_init(void)
{
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12,
    };

    ESP_ERROR_CHECK(adc_share_config_channel(ADC_CHAN, &config));
    ESP_ERROR_CHECK(dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE));
    dsps_wind_hann_f32(window, SAMPLES);

    xTaskCreate(tuning_task, "tuning_task", 8192, NULL, 5, NULL);
}
