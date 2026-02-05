#include "audio.h"
#include "sd_card.h"

#include <stdio.h>
#include "esp_log.h"

void app_main(void)
{
    sd_card_mount();

    audio_init();
}