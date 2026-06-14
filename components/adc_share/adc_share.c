#include "adc_share.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_err.h"

adc_oneshot_unit_handle_t adc_handle = NULL;
static SemaphoreHandle_t adc_mutex = NULL;

void adc_share_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };

    adc_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&init_config, &adc_handle));
}

esp_err_t adc_share_config_channel(adc_channel_t channel, const adc_oneshot_chan_cfg_t *config)
{
    if (adc_handle == NULL || config == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (adc_mutex != NULL) {
        xSemaphoreTake(adc_mutex, portMAX_DELAY);
    }

    esp_err_t ret = adc_oneshot_config_channel(adc_handle, channel, config);

    if (adc_mutex != NULL) {
        xSemaphoreGive(adc_mutex);
    }

    return ret;
}

esp_err_t adc_share_read(adc_channel_t channel, int *raw)
{
    if (adc_handle == NULL || raw == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    if (adc_mutex != NULL) {
        xSemaphoreTake(adc_mutex, portMAX_DELAY);
    }

    esp_err_t ret = adc_oneshot_read(adc_handle, channel, raw);

    if (adc_mutex != NULL) {
        xSemaphoreGive(adc_mutex);
    }

    return ret;
}
