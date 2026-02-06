#include "adc_share.h"

adc_oneshot_unit_handle_t adc_handle = NULL;

void adc_share_init(void)
{
    adc_oneshot_unit_init_cfg_t init_config = { .unit_id = ADC_UNIT_1 };

    adc_oneshot_new_unit(&init_config, &adc_handle);
}