#include "audio.h"
#include "sd_card.h"
#include "piezo.h"
#include "adc_share.h"
#include "oled.h"
#include "sht30.h"
#include "wifi.h"
#include "ws.h"

#include <stdio.h>

void app_main(void)
{
    adc_share_init(); // adc 공유 초기화

    wifi_init(); // wifi 초기화

    ws_init(); // ws 초기화

    sd_card_mount(); // sd 카드 마운트 초기화

    audio_init(); // 오디오 초기화

    piezo_init(); // 진동 센서 초기화

    start_oled(); // OLED 테스크 실행

    start_sensor(); // sht 테스크 실행
}