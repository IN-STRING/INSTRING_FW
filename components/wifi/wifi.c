#include "wifi.h"

#include "esp_wifi.h"
#include "ws.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "freertos/event_groups.h"
#include "esp_log.h"

// 비밀 유지 ^^
#define SSID "SON"
#define PASSWORD "33483348"
#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t wifi_event_group;

static const char *TAG = "WIFI";

static void event_handler(void *arg, esp_event_base_t event, int32_t id, void *data)
{
    if(event == WIFI_EVENT && id == WIFI_EVENT_STA_START) esp_wifi_connect();
    else if(event == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI(TAG, "reconnecting..");
    }
    else if(event == IP_EVENT && id == IP_EVENT_STA_GOT_IP) { // IP를 받은 뒤에 ws 연결 시작
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) data;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        ESP_LOGI(TAG, "IP : " IPSTR, IP2STR(&event->ip_info.ip));
        ws_init(); // ws 초기화
    }
}

void wifi_init(void)
{
    // WS 실행을 막기위한 eventgroup 생성
    wifi_event_group = xEventGroupCreate();

    // nvs 초기화
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 네트워크, 이벤트 루프 초기화
    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_sta();

    // wifi 초기 설정
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);


    // 핸들러 등록
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, NULL);

    // id, password 설정
    wifi_config_t config = {
        .sta = {
            .ssid = SSID,
            .password = PASSWORD,
        },
    };
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &config);

    esp_wifi_start();

    // 연결 될때까지 대기
    EventBits_t bits = xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT) ESP_LOGI(TAG, "waiting end");
}