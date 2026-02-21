#pragma GCC diagnostic ignored "-Wdeprecated-declarations" // 오류 지우기 용
#include "ssd1306.h"
#include "audio.h"
#include "piezo.h"
#include "effect.h"
#include "oled.h"

#include <stdio.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#define SCL 9
#define SDA 8 
#define NUM I2C_NUM_0

extern volatile float diff;
extern volatile int note_idx;
extern volatile bool recording;
extern const char* note_names[];
extern volatile effect_mode_t current_mode;

static ssd1306_handle_t oled_dev = NULL;

static const char *TAG = "OLED";

static void oled_init(void)
{
    i2c_config_t config = { // 구조체 선언(설정)
        .mode = I2C_MODE_MASTER,
        .sda_io_num = SDA,
        .scl_io_num = SCL,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 400000
    };
    
    i2c_param_config(NUM, &config);
    i2c_driver_install(NUM, config.mode, 0, 0, 0);
}

static void draw_tuner(int note_idx, float diff)
{
    if (note_idx < 0 || note_names[note_idx] == NULL) return; // 예외 처리

    char note_buf[8];
    sprintf(note_buf, "%s", note_names[note_idx]);

    // 공식 라이브러리 커스텀 폰트가 없다면 위치 조절로 크게 보이게 처리
    ssd1306_draw_string(oled_dev, 56, 4, (uint8_t *)note_buf, 16, 1);

    // 눈금
    ssd1306_draw_line(oled_dev, 14, 40, 114, 40); // 메인 가로선
    ssd1306_draw_line(oled_dev, 64, 35, 64, 45); // 정중앙 세로선
    ssd1306_draw_line(oled_dev, 14, 38, 14, 42); // 왼쪽 끝
    ssd1306_draw_line(oled_dev, 114, 38, 114, 42); // 오른쪽 끝

    // 커서 위치
    int offset_pixel = (int)(diff * 8); 
    if (offset_pixel > 50) offset_pixel = 50;
    if (offset_pixel < -50) offset_pixel = -50;

    int cursor_x = 64 + offset_pixel;

    // 커서 그리기
    ssd1306_draw_line(oled_dev, cursor_x, 32, cursor_x, 38); 
    ssd1306_draw_line(oled_dev, cursor_x - 2, 32, cursor_x + 2, 32);

    // 하단 상태 텍스트
    if (fabsf(diff) < 0.5f) ssd1306_draw_string(oled_dev, 44, 52, (uint8_t *)"PERFECT", 12, 1);
    else if (diff > 0) ssd1306_draw_string(oled_dev, 85, 52, (uint8_t *)"HIGH", 12, 1);
    else ssd1306_draw_string(oled_dev, 20, 52, (uint8_t *)"LOW", 12, 1);
}

static void oled_task(void *pram)
{
    static float smooth_diff = 0;

    while(1) {
        ssd1306_clear_screen(oled_dev, false);

        smooth_diff = smooth_diff * 0.8f + diff * 0.2f; 

        if (current_mode == FX_TUNER) {
            // 튜너 전용 화면
            if (note_idx == -1) ssd1306_draw_string(oled_dev, 25, 25, (uint8_t *)"Ready to Tune", 12, 1);
            else draw_tuner(note_idx, diff);
        }
        else {
            // 이펙터 상태 화면
            char mode_name[20];
            switch(current_mode) {
                case FX_OVERDRIVE: strcpy(mode_name, "OVERDRIVE"); break;
                case FX_DISTORTION: strcpy(mode_name, "DISTORTION"); break;
                case FX_DELAY: strcpy(mode_name, "DELAY"); break;
                default: strcpy(mode_name, "CLEAN"); break;
            }
            ssd1306_draw_string(oled_dev, 10, 10, (uint8_t *)"EFFECT MODE", 12, 1);
            ssd1306_draw_string(oled_dev, 10, 30, (uint8_t *)mode_name, 16, 1);
            
            if(recording) ssd1306_draw_string(oled_dev, 10, 52, (uint8_t *)"● RECORDING", 12, 1);
        }

        ssd1306_refresh_gram(oled_dev);

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void start_oled(void)
{
    oled_init();

    if (oled_dev == NULL) {
        oled_dev = ssd1306_create(NUM, SSD1306_I2C_ADDRESS);
    }

    if (oled_dev == NULL) {
        ESP_LOGE(TAG, "OLED create failed");
        return;
    }

    ssd1306_clear_screen(oled_dev, false);
    ssd1306_refresh_gram(oled_dev);

    ESP_LOGI(TAG, "OLED initialized");

    xTaskCreate(oled_task, "oled_task", 4096, NULL, 3, NULL);
}