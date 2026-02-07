#include "sht30.h"
#include "ws.h"

#include <math.h>
#include "driver/i2c.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#define ADDR 0x44
#define NUM I2C_NUM_0

static float temp = 0; // 온도
static float humi = 0; // 습도

static const char *TAG = "SHT30";

void sht30_task(void *pram) // 캡슐화 ㄴㄴ (서버코드를 여따 넣어야함)
{
    while(1) {
        uint8_t cmd[2] = {0x2C, 0x06};

        if (i2c_master_write_to_device(NUM, ADDR, cmd, 2, pdMS_TO_TICKS(100)) == ESP_OK) {
            
            vTaskDelay(pdMS_TO_TICKS(20)); // 측정 대기

            uint8_t data[6];
            if (i2c_master_read_from_device(NUM, ADDR, data, 6, pdMS_TO_TICKS(100)) == ESP_OK) {
                // 값 넣어주기
                uint16_t raw_temp = (data[0] << 8) | data[1];
                uint16_t raw_humi = (data[3] << 8) | data[4];

                // 계산식 적용
                temp = -45.0f + 175.0f * ((float)raw_temp / 65535.0f);
                humi = 100.0f * ((float)raw_humi / 65535.0f);

                ESP_LOGI(TAG, "Temp: %.2f C, Humi: %.2f %%", temp, humi);

                send_sensor_data(temp, humi);
                ESP_LOGI(TAG, "ws sensor data send done");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10000)); // 서버 주기에 맞춰 대기 (변경 가능)
    }
}

void start_sensor(void) {
    xTaskCreate(sht30_task, "sensor_task", 4096, NULL, 2, NULL); // 중요도가 좀 낮음
}