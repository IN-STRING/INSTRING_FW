idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES sd_card audio piezo adc_share oled
)