#pragma once

#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"

extern adc_oneshot_unit_handle_t adc_handle;

void adc_share_init(void);
esp_err_t adc_share_config_channel(adc_channel_t channel, const adc_oneshot_chan_cfg_t *config);
esp_err_t adc_share_read(adc_channel_t channel, int *raw);
