#include "audio.h"
#include "sd_card.h"
#include "effect.h"
#include "adc_share.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#include "esp_log.h"

#define I2S_WS 16
#define I2S_SCK 18
#define I2S_SD_IN 15
#define I2S_SD_OUT 17
#define I2S_PORT I2S_NUM_0

#define LED 2 // 녹음 중임을 알리는 led
#define BUTTON 5 // 녹음 및 모드 변경 버튼
#define ADC_CH ADC_CHANNEL_6

#define RATE 44100 // 44.1kHz
#define BUFFER_SIZE 2048 // PCM 16bit 샘플 개수

static const char *TAG = "AUDIO";

i2s_chan_handle_t rx_handle; // 녹음용 수신에 사용
i2s_chan_handle_t tx_handle; // 앰프 출력에 사용
SemaphoreHandle_t record_sign;
QueueHandle_t audio_queue;

volatile bool recording = false; // 녹음 진행중인지 확인
volatile bool stop = false; // 녹음 중단 요청
volatile effect_mode_t current_mode = FX_CLEAN; // 초기 모드 설정
static uint32_t press_start_time = 0;
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

    adc_oneshot_config_channel(adc_handle, ADC_CH, &config);
}

static void update_gain(void)
{
    int adc_raw;
    adc_oneshot_read(adc_handle, ADC_CH, &adc_raw);

    // gain 범위: 0.2 ~ 2.0
    gain = 0.2f + ((float)adc_raw / 4095.0f) * 1.8f;
}

static void record_task(void *parm)
{
    char filepath[64]; // 보내는 내용을 담는 배열
    filename(filepath, sizeof(filepath));

    FILE *f = fopen(filepath, "wb"); // 파일 열기

    if(!f) { // 예외 처리
        ESP_LOGE(TAG, "FILE OPEN FAIL");
        recording = false;
        vTaskDelete(NULL);
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

    // 종료 시 헤더 업데이트
    header.subchunk2Size = total_bytes;
    header.chunkSize = 36 + total_bytes;
    fseek(f, 0, SEEK_SET);
    fwrite(&header, sizeof(header), 1, f);
    fclose(f);

    ESP_LOGI(TAG, "녹음 완료. 총 용량: %d bytes", total_bytes);
    recording = false;
    vTaskDelete(NULL);
}

static void audio_task(void *pram)
{
    int32_t *raw_buf = malloc(BUFFER_SIZE * sizeof(int32_t));
    int16_t *pcm_buf = malloc(BUFFER_SIZE * sizeof(int16_t));
    size_t bytes_read, bytes_written;

    i2s_channel_enable(rx_handle);
    i2s_channel_enable(tx_handle);

    while(1) {
        update_gain();
        
        if(i2s_channel_read(rx_handle, raw_buf, 4096, &bytes_read, portMAX_DELAY) == ESP_OK){
            int samples = bytes_read / sizeof(int32_t);

            for(int i = 0; i < samples; i++) {
                int32_t sample = (int16_t)(raw_buf[i] >> 16); // 32bit -> 16bit 똥값 지우기 위해서 조절시 입력 소리 조절 가능

                sample = (int32_t)((float)sample * gain);

                // 클리핑 방지
                if(sample > 32767) sample = 32767;
                if(sample < -32768) sample = -32768;

                pcm_buf[i] = (int16_t)sample;
            }

            // 이펙터 걸기
            effector_apply(pcm_buf, samples, current_mode);
            
            // 실시간 출력
            i2s_channel_write(tx_handle, pcm_buf, samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);

            // 녹음
            if (recording) {
                int16_t *copy_buf = malloc(samples * sizeof(int16_t));

                if (copy_buf) {
                    memcpy(copy_buf, pcm_buf, samples * sizeof(int16_t));
                    if (xQueueSend(audio_queue, &copy_buf, 0) != pdTRUE) free(copy_buf); // 큐가 꽉 찼으면 메모리 해제
                }
            }
        }
    }
}

void record_control_task(void *pram) 
{
    while(1) {
        if(xSemaphoreTake(record_sign, portMAX_DELAY) == pdTRUE) {
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
}

// 우선 처리를 위한 ISR 처리
static void IRAM_ATTR button_isr_handler(void *arg) // 반응 속도를 끌어올리기 위해 IRAM위에 올려 처리
{
    uint32_t now = xTaskGetTickCountFromISR();
    int level = gpio_get_level(BUTTON);

    if(level == 0) { // 버튼을 눌렀을 때
        press_start_time = now;
    }
    else {
        uint32_t press_duration = now - press_start_time; // 누르고 있던 시간

        if(press_duration < pdMS_TO_TICKS(50)) return; // 예외 처리 (짧게 누르면 노이즈로 판단)

        if(press_duration < pdMS_TO_TICKS(500)) { // 0.5초 미만
            current_mode = (current_mode + 1) % FX_MODE_MAX; // 마지막 모드에 도달했을때 처음으로 돌리기 위해 나머지 연산자 활용
        }
        else { // 0.5초 이상
            BaseType_t noHightask = pdFALSE;
            xSemaphoreGiveFromISR(record_sign, &noHightask); // 더 높은 우선순위를 가진 테스크가 깨어나지 않음을 판단

            if (noHightask) {
                portYIELD_FROM_ISR();
            }
        }
    }
}

// 버튼 초기화
static void button_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << BUTTON,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE
    };

    gpio_config(&io_conf);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(BUTTON, button_isr_handler, NULL);
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

// 마이크, 스피커 초기화 및 버튼 초기화 실행
void audio_init(void)
{
    audio_queue = xQueueCreate(50, sizeof(int16_t *));

    record_sign = xSemaphoreCreateBinary(); // 녹음 시작을 알릴 세마포어 생성
    xTaskCreate(record_control_task, "record_ctrl", 2048, NULL, 4, NULL); // 관리 테스크 생성
    led_init();
    button_init();
    adc_init();
    effector_init(RATE);

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_PORT, I2S_ROLE_MASTER);
    i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle);

    i2s_std_config_t std_rx_cfg = { // 수신 기본 설정
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_MONO), // 32bit 입력
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK,
            .ws = I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = I2S_SD_IN
        },
    };

    i2s_std_config_t std_tx_cfg = { // 송신 기본 설정
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO), // 16bit 출력
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_SCK,
            .ws = I2S_WS,
            .dout = I2S_SD_OUT,
            .din = I2S_GPIO_UNUSED
        },
    };

    i2s_channel_init_std_mode(rx_handle, &std_rx_cfg);
    i2s_channel_init_std_mode(tx_handle, &std_tx_cfg);

    xTaskCreate(audio_task, "audio_task", 4096, NULL, 10, NULL); // task 생성

    ESP_LOGI(TAG, "audio init done");
}