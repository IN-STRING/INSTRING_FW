#include "sd_card.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "esp_log.h"

static const char *TAG = "SD_CARD";

// 파일 이름 자동 생성 함수 (audio.c에서 호출됨)
void filename(char *path, size_t len)
{
    int idx = 0;
    struct stat st;
    
    while (1) {
        // /sdcard/rec_000.wav 형식으로 이름 생성
        snprintf(path, len, "/sdcard/rec_%03d.wav", idx);
        
        // 해당 이름의 파일이 이미 존재하는지 확인
        if (stat(path, &st) != 0) {
            // 파일이 없으면 이 이름을 사용 (루프 종료)
            break;
        }
        idx++;
        
        // 안전장치: 파일이 너무 많아지면 중단
        if (idx > 999) break;
    }
    ESP_LOGI(TAG, "Generated filename: %s", path);
}

esp_err_t sd_card_mount(void)
{
    ESP_LOGI(TAG, "Initializing SD card...");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, // 마운트 실패 시 포맷 시도
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    sdmmc_card_t *card;
    const char mount_point[] = "/sdcard";

    // SDMMC 호스트 및 슬롯 설정 (S3 내장 슬롯 기준)
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1; // 1-bit 모드

    slot_config.clk = GPIO_NUM_39; 
    slot_config.cmd = GPIO_NUM_38;
    slot_config.d0  = GPIO_NUM_40;
    
    // 내부 풀업 저항 활성화
    slot_config.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

    ESP_LOGI(TAG, "Mounting filesystem...");
    esp_err_t ret = esp_vfs_fat_sdmmc_mount(mount_point, &host, &slot_config, &mount_config, &card);

    if (ret != ESP_OK) {
        if (ret == ESP_FAIL) {
            ESP_LOGE(TAG, "Failed to mount filesystem. If you want the card to be formatted, set format_if_mount_failed = true.");
        } else {
            ESP_LOGE(TAG, "Failed to initialize the card (%s). Make sure SD card lines have pull-up resistors in place.", esp_err_to_name(ret));
        }
        return ret;
    }

    ESP_LOGI(TAG, "Filesystem mounted");
    
    // 카드 정보 출력
    sdmmc_card_print_info(stdout, card);

    return ESP_OK;
}