#include "audio.h"
#include "sd_card.h"
#include "piezo.h"
#include "adc_share.h"
#include "oled.h"

#include <stdio.h>
#include "esp_log.h"

void app_main(void)
{
    adc_share_init();
    sd_card_mount();
    audio_init();
    piezo_init();
    start_oled();
}