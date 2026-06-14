#include "audio.h"
#include "sd_card.h"
#include "effect.h"
#include "piezo.h"
#include "oled.h"
#include "adc_share.h"
#include "ws.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#include "esp_log.h"

#define I2S_WS 13
#define I2S_SCK 12
#define I2S_SD_IN 15
#define I2S_SD_OUT 17
#define I2S_PORT I2S_NUM_0

#define LED 6 // 녹음 중임을 알리는 led
#define BUTTON 5 // 녹음 및 모드 변경 버튼
#define ADC_CH ADC_CHANNEL_6

#define RATE 44100 // 44.1kHz
// 한 번에 처리할 모노 샘플 수. RX DMA 링버퍼(약 33ms)보다 충분히 작아야
// TX 쓰기가 블로킹되는 동안 RX가 넘쳐서 샘플이 유실되는 것을 막을 수 있음
#define BUFFER_SIZE 512

// 출력 감쇠와 최대 게인을 낮춰 마이크→스피커 루프 게인을 1 미만으로 억제(하울링 방지)
#define OUTPUT_ATTENUATION 0.18f
#define MIN_GAIN 0.25f
#define MAX_GAIN 4.0f

// 하울링(음향 피드백) 억제 노치 필터. 측정상 8.3kHz에서 자가발진하므로 그 대역만 제거.
// Q를 낮추면 더 넓은 대역을 깎아 안정적이지만 고음이 둔해짐 (발진 주파수가 옮겨가면 조정)
#define NOTCH_FREQ 8300.0f
#define NOTCH_Q 4.0f

// DC 차단(1차 고역통과) 필터 계수. R=0.995 → 차단주파수 약 35Hz.
// INMP441의 DC 오프셋과 저주파 idle 잡음을 제거 (저음 E현 82Hz는 보존)
#define DC_BLOCK_R 0.995f

// 노이즈 게이트(출력 경로 전용). 마이크가 조용할 때 스피커를 무음 처리.
// 히스테리시스로 채터링 방지: OPEN을 넘으면 열리고 CLOSE 밑으로 내려가면 닫힘
#define GATE_OPEN_LEVEL 300
#define GATE_CLOSE_LEVEL 150

#define ENABLE_STARTUP_TONE 1
#if ENABLE_STARTUP_TONE
#define STARTUP_TONE_MS 700
#define STARTUP_TONE_HZ 440.0f
#define STARTUP_TONE_LEVEL 3000
#define TWO_PI 6.28318530718f
#endif

static const char *TAG = "AUDIO";

i2s_chan_handle_t rx_handle; // 녹음용 수신에 사용
i2s_chan_handle_t tx_handle; // 앰프 출력에 사용
QueueHandle_t audio_queue;

volatile bool recording = false; // 녹음 진행중인지 확인
volatile bool stop = false; // 녹음 중단 요청
volatile effect_mode_t current_mode = FX_NONE; // 초기 모드 설정
volatile float gain = 1.0f;

typedef struct {
    char     chunkID[4];        // "RIFF"
    int32_t  chunkSize;
    char     format[4];         // "WAVE"
    char     subchunk1ID[4];    // "fmt "
    int32_t  subchunk1Size;     // 16
    int16_t  audioFormat;       // 1 = PCM
    int16_t  numChannels;       // 1
    int32_t  sampleRate;
    int32_t  byteRate;
    int16_t  blockAlign;
    int16_t  bitsPerSample;     // 16
    char     subchunk2ID[4];    // "data"
    int32_t  subchunk2Size;
} __attribute__((packed)) WavHeader;

static void adc_init(void)
{
    adc_oneshot_chan_cfg_t config = {
        .bitwidth = ADC_BITWIDTH_12,
        .atten = ADC_ATTEN_DB_12
    };

    ESP_ERROR_CHECK(adc_share_config_channel(ADC_CH, &config));
}

static void update_gain(void)
{
    int adc_raw;
    if (adc_share_read(ADC_CH, &adc_raw) != ESP_OK) {
        return;
    }

    // MIN_GAIN ~ MAX_GAIN 범위로 제한
    float target = MIN_GAIN + ((float)adc_raw / 4095.0f) * (MAX_GAIN - MIN_GAIN);

    // ADC 지터로 게인이 버퍼마다 튀면 경계에서 클릭(지퍼 노이즈)이 발생하므로 완만하게 추종
    gain = gain * 0.8f + target * 0.2f;
}

#if ENABLE_STARTUP_TONE
static esp_err_t write_startup_tone(int32_t *raw_buf, size_t *bytes_written)
{
    int total_samples = (RATE * STARTUP_TONE_MS) / 1000;
    int phase = 0;

    while (total_samples > 0) {
        int samples = total_samples > BUFFER_SIZE ? BUFFER_SIZE : total_samples;

        for (int i = 0; i < samples; i++) {
            float t = (float)(phase++) / (float)RATE;
            int16_t sample = (int16_t)(sinf(TWO_PI * STARTUP_TONE_HZ * t) * STARTUP_TONE_LEVEL);
            raw_buf[2 * i] = ((int32_t)sample) << 16;
            raw_buf[2 * i + 1] = ((int32_t)sample) << 16;
        }

        esp_err_t ret = i2s_channel_write(tx_handle, raw_buf, samples * 2 * sizeof(int32_t), bytes_written, portMAX_DELAY);
        if (ret != ESP_OK) {
            return ret;
        }
        total_samples -= samples;
    }

    return ESP_OK;
}
#endif

static void record_task(void *parm)
{
    char filepath[64];
    filename(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "wb"); // 파일 열기

    if(!f) { // 예외 처리
        ESP_LOGE(TAG, "FILE OPEN FAIL");
        recording = false;
        vTaskDelete(NULL);
        return;
    }

    WavHeader header = {
        .chunkID = {'R','I','F','F'},
        .format  = {'W','A','V','E'},
        .subchunk1ID = {'f','m','t',' '},
        .subchunk2ID = {'d','a','t','a'},
        .subchunk1Size = 16,
        .audioFormat = 1,
        .numChannels = 1,
        .sampleRate = RATE,
        .bitsPerSample = 16,
        .byteRate = RATE * 1 * 16 / 8,
        .blockAlign = 1 * 16 / 8,
        .subchunk2Size = 0,
        .chunkSize = 36,
    };

    fwrite(&header, sizeof(header), 1, f);
    int total_bytes = 0;
    int16_t *data_to_write;

    ESP_LOGI(TAG, "recording file : %s", filepath);
    ESP_LOGI(TAG, "recording start");

    while (!stop) {
        // 큐에서 데이터를 받을 때까지 대기
        if (xQueueReceive(audio_queue, &data_to_write, pdMS_TO_TICKS(1000)) == pdTRUE) {
            size_t written = fwrite(data_to_write, 1, BUFFER_SIZE * sizeof(int16_t), f);
            total_bytes += written;
            free(data_to_write); // 전달받은 버퍼 해제
        }
    }

    // 메모리 누수 방지: 녹음이 끝난 후 큐에 남은 데이터 비우기
    while (xQueueReceive(audio_queue, &data_to_write, 0) == pdTRUE) {
        free(data_to_write);
    }

    // 종료 시 헤더 업데이트
    header.subchunk2Size = total_bytes;
    header.chunkSize = 36 + total_bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, f);
    fclose(f);

    ESP_LOGI(TAG, "recording success, total bytes : %d bytes", total_bytes);
    recording = false;

    // 녹음 종료 후 서버로 업로드
    ESP_LOGI(TAG, "uploading start");
    send_record_file(filepath);

    vTaskDelete(NULL);
}

static void audio_task(void *pram)
{
    // RX는 스테레오 프레임(L,R 슬롯 2개)으로 수신하므로 모노 샘플당 int32 2개 필요
    int32_t *raw_buf = malloc(BUFFER_SIZE * 2 * sizeof(int32_t));
    int16_t *pcm_buf = malloc(BUFFER_SIZE * sizeof(int16_t));
    size_t bytes_read, bytes_written;

    if (raw_buf == NULL || pcm_buf == NULL) {
        ESP_LOGE(TAG, "audio buffer allocation failed");
        free(raw_buf);
        free(pcm_buf);
        vTaskDelete(NULL);
        return;
    }

    esp_err_t ret = i2s_channel_enable(rx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S RX enable failed: %s", esp_err_to_name(ret));
        free(raw_buf);
        free(pcm_buf);
        vTaskDelete(NULL);
        return;
    }

    ret = i2s_channel_enable(tx_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2S TX enable failed: %s", esp_err_to_name(ret));
        free(raw_buf);
        free(pcm_buf);
        vTaskDelete(NULL);
        return;
    }

    #if ENABLE_STARTUP_TONE
    ret = write_startup_tone(raw_buf, &bytes_written);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "startup tone failed: %s", esp_err_to_name(ret));
    }
    #endif

    // DC 차단 필터 상태 (이전 입력/출력 샘플)
    float dc_x1 = 0.0f, dc_y1 = 0.0f;
    // 노이즈 게이트 상태 (엔벨로프 추종기 + 게이트 게인)
    float env = 0.0f, gate_gain = 0.0f;
    bool gate_open = false;

    // 하울링 억제 노치 필터 계수 (RBJ biquad notch) + 상태
    float w0 = 6.28318530718f * NOTCH_FREQ / (float)RATE;
    float cw = cosf(w0);
    float alpha = sinf(w0) / (2.0f * NOTCH_Q);
    float a0 = 1.0f + alpha;
    float nb0 = 1.0f / a0, nb1 = (-2.0f * cw) / a0, nb2 = 1.0f / a0;
    float na1 = (-2.0f * cw) / a0, na2 = (1.0f - alpha) / a0;
    float nx1 = 0.0f, nx2 = 0.0f, ny1 = 0.0f, ny2 = 0.0f;

    while(1) {
        update_gain();

        if(i2s_channel_read(rx_handle, raw_buf, BUFFER_SIZE * 2 * sizeof(int32_t), &bytes_read, portMAX_DELAY) == ESP_OK){
            int samples = bytes_read / (2 * sizeof(int32_t));

            for(int i = 0; i < samples; i++) {
                // 스테레오 프레임에서 마이크 데이터가 있는 왼쪽 슬롯(짝수 인덱스)만 사용.
                // INMP441은 32비트 슬롯의 상위 24비트에 MSB 정렬로 출력하므로 >> 14로
                // 16비트 PCM 변환 + 약 +12dB 보정 (소리가 작으면 12, 크면 16으로 조절)
                float x = (float)(raw_buf[2 * i] >> 14);

                // DC 차단: y[n] = x[n] - x[n-1] + R*y[n-1]
                float y = x - dc_x1 + DC_BLOCK_R * dc_y1;
                dc_x1 = x;
                dc_y1 = y;

                int32_t sample = (int32_t)(y * gain);

                if(sample > 32767)  sample = 32767;
                if(sample < -32768) sample = -32768;

                pcm_buf[i] = (int16_t)sample;
            }

            if (current_mode != FX_TUNER) {
                if (current_mode != FX_NONE) {
                    effector_apply(pcm_buf, samples, current_mode);
                }

                for(int i = 0; i < samples; i++) {
                    // 노이즈 게이트: 마이크가 조용하면 스피커를 무음 처리 (녹음 데이터는 원본 유지)
                    float a = fabsf((float)pcm_buf[i]);
                    if (a > env) env = a;            // 빠른 어택
                    else env *= 0.9995f;             // 느린 릴리즈 (~45ms)

                    if (env > GATE_OPEN_LEVEL) gate_open = true;
                    else if (env < GATE_CLOSE_LEVEL) gate_open = false;

                    // 열림은 빠르게, 닫힘은 천천히 램프해서 클릭 방지
                    float target = gate_open ? 1.0f : 0.0f;
                    float coef = (target > gate_gain) ? 0.25f : 0.0008f;
                    gate_gain += (target - gate_gain) * coef;

                    // 출력 레벨 감쇠 + 게이트 적용
                    float xin = (float)pcm_buf[i] * OUTPUT_ATTENUATION * gate_gain;

                    // 노치 필터로 하울링 주파수 제거 (Direct Form I)
                    float yout = nb0 * xin + nb1 * nx1 + nb2 * nx2 - na1 * ny1 - na2 * ny2;
                    nx2 = nx1; nx1 = xin;
                    ny2 = ny1; ny1 = yout;

                    int32_t val = (int32_t)yout;
                    if (val > 32767)  val = 32767;
                    if (val < -32768) val = -32768;

                    // 좌/우 슬롯에 같은 샘플 복제
                    raw_buf[2 * i] = val << 16;
                    raw_buf[2 * i + 1] = val << 16;
                }

                ret = i2s_channel_write(tx_handle, raw_buf, samples * 2 * sizeof(int32_t), &bytes_written, portMAX_DELAY);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "I2S write failed: %s", esp_err_to_name(ret));
                    vTaskDelay(pdMS_TO_TICKS(10));
                }
            }

            if (recording) {
                static int log_cnt = 0;
                if (log_cnt++ % 50 == 0) { 
                    ESP_LOGD(TAG, "recording.."); 
                }

                int16_t *copy_buf = malloc(samples * sizeof(int16_t));

                if (copy_buf) {
                    memcpy(copy_buf, pcm_buf, samples * sizeof(int16_t));
                    if (xQueueSend(audio_queue, &copy_buf, 0) != pdTRUE) free(copy_buf); 
                }
            }
        }
    }
}

// 새로 작성된 폴링 방식의 버튼 컨트롤 태스크 (안정성 극대화)
void record_control_task(void *pram) 
{
    int last_state = 1; // 풀업 상태이므로 기본값은 HIGH(1)
    uint32_t press_time = 0;

    while(1) {
        int current_state = gpio_get_level(BUTTON);

        // 버튼을 눌렀을 때 (HIGH -> LOW)
        if(last_state == 1 && current_state == 0) {
            press_time = xTaskGetTickCount();
        }
        // 버튼에서 손을 뗐을 때 (LOW -> HIGH)
        else if(last_state == 0 && current_state == 1) {
            // 누르고 있던 시간을 밀리초(ms)로 계산
            uint32_t duration = (xTaskGetTickCount() - press_time) * portTICK_PERIOD_MS;

            if (duration > 50 && duration < 500) { // 0.5초 미만 (이펙터 모드 변경)
                current_mode = (current_mode + 1) % FX_MODE_MAX;
                ESP_LOGI(TAG, "AMP MODE CHANGED: %d", current_mode);
            }
            else if (duration >= 500) { // 0.5초 이상 (녹음 시작/중지)
                ESP_LOGI(TAG, "Button long pressed! Current recording state: %d", recording);

                if(!recording) {
                    recording = true;
                    stop = false;
                    xTaskCreate(record_task, "record_task", 8192, NULL, 5, NULL);
                    gpio_set_level(LED, 1);
                    ESP_LOGI(TAG, "recording start sign send");
                }
                else {
                    stop = true;
                    gpio_set_level(LED, 0);
                    ESP_LOGI(TAG, "recording stop sign send");
                }
            }
        }

        last_state = current_state;
        vTaskDelay(pdMS_TO_TICKS(10)); // 10ms 단위로 감시 (CPU 과부하 방지)
    }
}

// 인터럽트 비활성화된 버튼 초기화
static void button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE // 인터럽트 충돌 방지!
    };
    gpio_config(&io_conf);
}

// led 초기화
static void led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << LED,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
    };
    gpio_config(&io_conf);
}

// 마이크, 스피커 초기화 및 태스크 실행
void audio_init(void)
{
    // 버퍼가 작아진 만큼 큐 깊이를 늘려 SD 쓰기 지연에 대한 여유 유지 (~1.4초)
    audio_queue = xQueueCreate(120, sizeof(int16_t *));

    // 세마포어 생성 제거 및 태스크 실행
    xTaskCreate(record_control_task, "record_ctrl", 2048, NULL, 4, NULL); 
    
    led_init();
    button_init();
    adc_init();
    effector_init(RATE);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));

    // RX는 STEREO로 수신 후 소프트웨어에서 왼쪽 슬롯만 추출.
    // (MONO 모드에서는 슬롯 마스킹이 동작하지 않아 반대쪽 슬롯의 쓰레기 값이
    //  샘플 사이에 끼어들어 노이즈가 발생함)
    i2s_std_config_t std_rx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_SD_IN
        },
    };

    // TX도 STEREO로 보내고 좌/우에 같은 샘플을 복제 (RX와 동일하게 MONO 슬롯
    // 마스킹 문제를 피하고, 듀플렉스로 클럭을 공유하는 RX와 설정을 일치시킴)
    i2s_std_config_t std_tx_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK,
            .ws = I2S_WS,
            .dout = I2S_SD_OUT,
            .din = I2S_GPIO_UNUSED
        },
    };

    ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_rx_cfg));
    ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_tx_cfg));

    xTaskCreate(audio_task, "audio_task", 4096, NULL, 10, NULL); 

    ESP_LOGI(TAG, "audio init done");
}
