#include "ws.h"

#include "esp_websocket_client.h"
#include <sys/stat.h>
#include "esp_log.h"

// 비밀 유지 ^^
#define SERVER_URI ""

esp_websocket_client_handle_t client = NULL;

static const char *TAG = "Websocket";

static void websocket_event_handler(void *arg, esp_event_base_t event, int32_t id, void *event_data)
{
    esp_websocket_event_data_t *data = (esp_websocket_event_data_t *)event_data;
    switch(id) {
        case WEBSOCKET_EVENT_CONNECTED:
            ESP_LOGI(TAG, "WS connected");
            break;
        case WEBSOCKET_EVENT_DATA:
            ESP_LOGI(TAG, "received : %.*s", data->data_len, (char *)data->data_ptr);
            break;
        case WEBSOCKET_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "WS disconnected");
            break;
    }
}

void send_sensor_data(float temp, float humi) {
    if (esp_websocket_client_is_connected(client)) {
        char json_str[64];
        snprintf(json_str, sizeof(json_str), "{\"temp\": %.2f, \"humi\": %.2f}", temp, humi); // 서버랑 잘 맞추기
        esp_websocket_client_send_text(client, json_str, strlen(json_str), pdMS_TO_TICKS(1000));
    }
}

void send_record_file(const char* filepath) {
    if (client == NULL || !esp_websocket_client_is_connected(client)) {
        ESP_LOGE(TAG, "server not connected");
        return;
    }

    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "can't open file : %s", filepath);
        return;
    }

    // 파일 크기 확인
    struct stat st;
    stat(filepath, &st);
    int total_size = st.st_size;

    ESP_LOGI(TAG, "file send start : %s (%d bytes)", filepath, total_size);

    char *buffer = malloc(4096); // 4KB씩 나누어 전송
    int read_bytes;
    int sent_bytes = 0;

    while ((read_bytes = fread(buffer, 1, 4096, f)) > 0) {
        // WebSocket Binary 데이터로 전송
        esp_websocket_client_send_bin(client, buffer, read_bytes, pdMS_TO_TICKS(5000));
        sent_bytes += read_bytes;

        vTaskDelay(pdMS_TO_TICKS(10)); 
    }

    free(buffer);
    fclose(f);
    ESP_LOGI(TAG, "file send sussce!");
}

void ws_init(void)
{
    // 주소 설정
    esp_websocket_client_config_t config = {
        .uri = SERVER_URI
    };

    client = esp_websocket_client_init(&config);
    esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event_handler, (void *)client);
    esp_websocket_client_start(client);
}