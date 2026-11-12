#ifndef POWER_MANAGEMENT_TASK_H_
#define POWER_MANAGEMENT_TASK_H_

#include <stdbool.h>
#include <stdint.h>

typedef struct
{
    float fan_perc;
    uint16_t fan_rpm;
    uint16_t fan2_rpm;
    float chip_temp_avg;
    float chip_temp2_avg;
    float vr_temp;
    float voltage;
    float frequency_value;
    float actual_frequency;    
    float expected_hashrate;
    float power;
    float current;
    float core_voltage;
    uint32_t cml_error_count;
    uint32_t watchdog_resets;
    volatile bool asic_watchdog_trigger;
    uint8_t watchdog_consecutive_failures;
    int64_t watchdog_last_failure_us;
    int64_t last_freq_change_us;
    float watchdog_freq_offset_mhz;
} PowerManagementModule;

void POWER_MANAGEMENT_init_frequency(void * pvParameters);

void POWER_MANAGEMENT_task(void * pvParameters);

#endif
