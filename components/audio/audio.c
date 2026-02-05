#include "audio.h"
#include "sd_card.h"
#include "effect.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "driver/i2s_std.h"
#include "driver/gpio.h"

#include "esp_log.h"

#define I2S_WS 16
#define I2S_SCK 18
#define I2S_SD_IN 15
#define I2S_SD_OUT 17
#define I2S_PORT I2S_NUM_0

#define BUTTON 5 // 녹음 버튼

#define RATE 44100 // 44.1kHz
#define BUFFER_SIZE 2048 // PCM 16bit 샘플 개수

static const char *TAG = "AUDIO";

i2s_chan_handle_t rx_handle; // 녹음용 수신에 사용
i2s_chan_handle_t tx_handle; // 앰프 출력에 사용
QueueHandle_t audio_queue;

volatile bool recording = false; // 녹음 진행중인지 확인
volatile bool stop = false; // 녹음 중단 요청
volatile effect_mode_t current_mode = FX_CLEAN;
static uint32_t last_press = 0;

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

    while(1) // task에서 정지 명령을 받을때 까지 반복
    {
        if(i2s_channel_read(rx_handle, raw_buf, 4096, &bytes_read, portMAX_DELAY) == ESP_OK){
            int samples = bytes_read / sizeof(int32_t);

            for(int i = 0; i < samples; i++) pcm_buf[i] = (int16_t)(raw_buf[i] >> 16); // 32bit -> 16bit 똥값 지우기 위해서 조절시 입력 소리 조절 가능

            // 실시간 출력
            i2s_channel_write(tx_handle, pcm_buf, samples * sizeof(int16_t), &bytes_written, portMAX_DELAY);

            // 이펙터 걸기
            effector_apply(pcm_buf, samples, current_mode);
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

// 우선 처리를 위한 ISR 처리
static void IRAM_ATTR button_isr_handler(void *arg) // 반응 속도를 끌어올리기 위해 IRAM위에 올려 처리
{
    uint32_t now = xTaskGetTickCountFromISR();

    if(now - last_press > pdMS_TO_TICKS(300)) {
        if(!recording){
            recording = true;
            stop = false;
            xTaskCreate(record_task, "record_task", 8192, NULL, 5, NULL);
        }
        else stop = true;

        last_press = now;
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
        .intr_type = GPIO_INTR_NEGEDGE
    };

    gpio_config(&io_conf);
    gpio_install_isr_service(ESP_INTR_FLAG_IRAM);
    gpio_isr_handler_add(BUTTON, button_isr_handler, NULL);
}

// 마이크, 스피커 초기화 및 버튼 초기화 실행
void audio_init(void)
{
    audio_queue = xQueueCreate(10, sizeof(int16_t *));

    button_init();
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