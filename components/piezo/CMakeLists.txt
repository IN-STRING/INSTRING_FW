idf_component_register(
    SRCS "piezo.c"
    INCLUDE_DIRS "."
    REQUIRES esp_adc esp-dsp adc_share effect
)