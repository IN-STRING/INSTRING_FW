#include "sd_card.h"

#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "driver/sdmmc_defs.h"
#include "sdmmc_cmd.h"

#include <stdio.h>
#include <sys/stat.h>
#include "esp_log.h"

static const char *TAG = "SD_CARD";


esp_err_t sd_card_mount(void)
{
    ESP_LOGI(TAG, "sd card mount..");

    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true, // 마운트 실패 시 포멧
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };
    
    sdmmc_card_t *card;

    // SDMMC 호스트 설정
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = 1;
    
    esp_err_t ret = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot_config, &mount_config, &card);
    
    if(ret != ESP_OK) {
        ESP_LOGI(TAG, "SD CARD mount fail : %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "SD CARD mount sussce!");
    sdmmc_card_print_info(stdout, card);

    return ESP_OK;
}

// 파일 이름 자동 변경
void filename(char *path, size_t len)
{
    int idx = 0;
    struct stat st;
    
    while (1) {
        snprintf(path, len, "/sdcard/rec_%03d.wav", idx);
        if (stat(path, &st) != 0) break;
        idx++;
    }
}