idf_component_register(
    SRCS "audio.c"
    INCLUDE_DIRS "."
    REQUIRES driver sd_card effect esp_adc adc_share ws piezo oled
)