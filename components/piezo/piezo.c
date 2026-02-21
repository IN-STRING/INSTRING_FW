#include "piezo.h"
#include "adc_share.h"
#include "effect.h"

#include <stdio.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_adc/adc_oneshot.h"
#include "dsps_fft2r.h"
#include "dsps_wind.h"
#include "rom/ets_sys.h"
#include "esp_log.h"

#define SAMPLES 2048
#define SAMPLING_FREQ 4000
#define ADC_CHAN ADC_CHANNEL_4

static float fft_input[SAMPLES * 2]; // 실수와 허수 포함
static float window[SAMPLES];

static float guitar_notes[] = {82.41, 110.00, 146.83, 196.00, 246.94, 329.63};
const char* note_names[] = {"6E", "5A", "4D", "3G", "2B", "1E"};

volatile float diff = 0.0f;
volatile int note_idx = -1;
extern volatile effect_mode_t current_mode;

static const char *TAG = "PIZEO";

static int find_node(float freq)
{
    int closest = 0;
    float min_diff = 1000.0;
    for (int i = 0; i < 6; i++) {
        float diff = fabsf(freq - guitar_notes[i]); // 절댓값으로 계산
        if (diff < min_diff) {
            min_diff = diff;
            closest = i;
        }
    }
    return closest;
}

static void tuning_task(void *pram)
{
    while(1) {
        // 튜너 모드가 아니면 연산을 하지 않고 대기 (CPU 절약)
        if (current_mode != FX_TUNER) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        for(int i = 0; i < SAMPLES; i++) { // 샘플링
            int raw;
            adc_oneshot_read(adc_handle, ADC_CHAN, &raw);

            fft_input[i * 2] = (float)(raw - 2048) * window[i];
            fft_input[i * 2 + 1] = 0;

            ets_delay_us(1000000 / SAMPLING_FREQ);
        }

        // FFT 실행(주파수 분해)
        dsps_fft2r_fc32(fft_input, SAMPLES);
        dsps_bit_rev_fc32(fft_input, SAMPLES);
        dsps_cplx2reC_fc32(fft_input, SAMPLES);

        // 최대 주파수 찾기
        float max_mag = 0;
        int peak_idx = 0;
        for(int i = 10; i < SAMPLES / 2; i++) {
            float real = fft_input[i * 2];
            float imag = fft_input[i * 2 + 1];
            float mag = (real * real + imag * imag);

            // 찾으면 peek_idx에 넣기
            if(mag > max_mag) {
                max_mag = mag;
                peak_idx = i;
            }
        }

        if (max_mag > 100000.0) { // 임계값 이상일 때만
            float freq = (float)peak_idx * SAMPLING_FREQ / SAMPLES;
            note_idx = find_node(freq);
            float target_freq = guitar_notes[note_idx];
            diff = freq - target_freq;

            ESP_LOGI(TAG, "Note: %s | Freq: %.2f Hz | Diff: %+.2f Hz", note_names[note_idx], freq, diff);
            // 여기에 디스플레이 호출 예정
            
            if (fabsf(diff) < 0.5) ESP_LOGW(TAG, "PERFECT!");
        }
        else {
            note_idx = -1;
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void piezo_init(void)
{
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12
    };

    adc_oneshot_config_channel(adc_handle, ADC_CHAN, &config);

    dsps_fft2r_init_fc32(NULL, CONFIG_DSP_MAX_FFT_SIZE);
    dsps_wind_hann_f32(window, SAMPLES);

    xTaskCreate(tuning_task, "tuning_task", 8192, NULL, 5, NULL);
}