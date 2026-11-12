#include <math.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "global_state.h"
#include "nvs_config.h"
#include "vcore.h"
#include "thermal.h"
#include "power.h"
#include "asic.h"
#include "utils.h"
#include "asic_init.h"
#include "asic_reset.h"
#include "driver/uart.h"

#define POLL_RATE 100

// Hard Caps (physical safety limits in RAM)
#define VCORE_MIN_MV 1000
#define VCORE_MAX_MV 1250
#define FREQ_MIN_MHZ 400.0f
#define MAX_POWER_W 32.0f

// Thermal thresholds (Celsius)
#define ASIC_SOFT_THROTTLE_TEMP 68.0f
#define ASIC_SEVERE_THROTTLE_TEMP 72.0f
#define ASIC_EMERGENCY_TEMP 75.0f
#define ASIC_SAFE_RECOVERY_TEMP 65.0f

#define VR_SOFT_THROTTLE_TEMP 90.0f
#define VR_SEVERE_THROTTLE_TEMP 98.0f
#define VR_EMERGENCY_TEMP 105.0f
#define VR_SAFE_RECOVERY_TEMP 85.0f

// Soft-throttling and hysteresis timing / step sizes
#define THROTTLE_STEP_INTERVAL_US     (10LL * 1000000LL)
#define RECOVERY_HOLD_US              (30LL * 1000000LL)
#define RECOVERY_STEP_INTERVAL_US     (15LL * 1000000LL)
#define THROTTLE_FREQ_STEP_MHZ        12.5f
#define THROTTLE_SEVERE_FREQ_STEP_MHZ 25.0f
#define THROTTLE_VCORE_STEP_MV        10

#define WATCHDOG_FAILURE_WINDOW_US  (10LL * 60LL * 1000000LL)
#define WATCHDOG_FREQ_STEP_DOWN_MHZ 12.5f
#define WATCHDOG_MIN_FREQ_MHZ       FREQ_MIN_MHZ
#define WATCHDOG_RECOVERY_DELAY_MS  2000

static const char * TAG = "power_management";

static bool is_valid_temp(float temp)
{
    return !isnan(temp) && temp > 0.0f;
}

static uint16_t clamp_vcore_mv(uint16_t voltage_mv)
{
    if (voltage_mv < VCORE_MIN_MV) {
        return VCORE_MIN_MV;
    }
    if (voltage_mv > VCORE_MAX_MV) {
        return VCORE_MAX_MV;
    }
    return voltage_mv;
}

static uint16_t calculate_effective_voltage(uint16_t target_voltage_mv, uint16_t thermal_offset_mv)
{
    uint16_t clamped_target = clamp_vcore_mv(target_voltage_mv);
    if (thermal_offset_mv >= clamped_target) {
        return VCORE_MIN_MV;
    }
    return clamp_vcore_mv(clamped_target - thermal_offset_mv);
}

static float calculate_effective_frequency(float target_freq, float offset_mhz)
{
    return fmaxf(FREQ_MIN_MHZ, target_freq - offset_mhz);
}

static void reset_thermal_throttle_state(PowerManagementModule * power_management)
{
    power_management->throttle_active = false;
    power_management->thermal_offset_mhz = 0.0f;
    power_management->thermal_offset_mv = 0;
    power_management->last_throttle_step_us = 0;
    power_management->safe_zone_since_us = 0;
}

static void mining_stop(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Stopping mining");

    // Wind frequency down to 50 MHz before cutting power. This also updates
    // the transition tracker so the ramp starts from 50 MHz on next start,
    // rather than the stale pre-reset frequency.
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value = 50;
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.expected_hashrate = 0;

    ASIC_set_frequency(GLOBAL_STATE);
    ASIC_set_nonce_space(GLOBAL_STATE);

    // Cut ASIC power and hold in reset
    VCORE_set_voltage(GLOBAL_STATE, 0.0f);
    GLOBAL_STATE->POWER_MANAGEMENT_MODULE.actual_voltage_mv = 0;
    asic_hold_reset_low();

    // Mark uninitialized immediately so tasks stop issuing UART commands
    GLOBAL_STATE->ASIC_initalized = false;

    // Give tasks time to complete any in-progress UART operation
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Flush any stale data from the UART buffers
    uart_flush(UART_NUM_1);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    ESP_LOGI(TAG, "Mining stopped");
}

static uint8_t mining_start(GlobalState * GLOBAL_STATE)
{
    ESP_LOGI(TAG, "Starting mining");

    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    uint16_t target_voltage = GLOBAL_STATE->SELF_TEST_MODULE.is_active
                                  ? GLOBAL_STATE->DEVICE_CONFIG.family.asic.default_voltage_mv
                                  : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    power_management->target_voltage = target_voltage;

    uint16_t effective_voltage_mv = calculate_effective_voltage(target_voltage, power_management->thermal_offset_mv);
    VCORE_set_voltage(GLOBAL_STATE, (double) effective_voltage_mv / 1000.0);
    power_management->actual_voltage_mv = effective_voltage_mv;

    // Wait for voltage to stabilize before touching the ASIC
    vTaskDelay(500 / portTICK_PERIOD_MS);

    // Clear any accumulated UART garbage before init
    uart_flush(UART_NUM_1);
    vTaskDelay(100 / portTICK_PERIOD_MS);

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);
    // Stabilization delay of 2000ms prevents race conditions where tasks are
    // just starting to use the ASIC while power management tries to change frequency
    uint8_t chip_count = asic_initialize(GLOBAL_STATE, ASIC_INIT_RECOVERY, 2000);

    if (chip_count > 0) {
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.last_freq_change_us = esp_timer_get_time();
        ESP_LOGI(TAG, "Mining started successfully (%d chip(s))", chip_count);
    } else {
        ESP_LOGE(TAG, "Mining start failed - ASIC not detected");
    }

    return chip_count;
}

static float expected_hashrate(GlobalState * GLOBAL_STATE)
{
    return GLOBAL_STATE->POWER_MANAGEMENT_MODULE.frequency_value * GLOBAL_STATE->DEVICE_CONFIG.family.asic.small_core_count * GLOBAL_STATE->DEVICE_CONFIG.family.asic_count / 1000.0;
}

static void reset_watchdog_state(PowerManagementModule * power_management)
{
    power_management->watchdog_freq_offset_mhz = 0.0f;
    power_management->watchdog_consecutive_failures = 0;
    power_management->watchdog_last_failure_us = 0;
    power_management->asic_watchdog_trigger = false;
}

static void handle_asic_watchdog(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    int64_t now_us = esp_timer_get_time();

    if (power_management->watchdog_last_failure_us > 0 &&
        (now_us - power_management->watchdog_last_failure_us) < WATCHDOG_FAILURE_WINDOW_US) {
        power_management->watchdog_consecutive_failures++;
    } else {
        power_management->watchdog_consecutive_failures = 1;
    }
    power_management->watchdog_last_failure_us = now_us;

    if (power_management->watchdog_consecutive_failures >= 4) {
        ESP_LOGE(TAG, "ASIC Watchdog Level 3 (%u consecutive failures in <10 min) - restarting system",
                 power_management->watchdog_consecutive_failures);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    if (power_management->watchdog_consecutive_failures >= 2) {
        float nvs_freq = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
        if (nvs_freq > WATCHDOG_MIN_FREQ_MHZ) {
            float max_offset = nvs_freq - WATCHDOG_MIN_FREQ_MHZ;
            power_management->watchdog_freq_offset_mhz = fminf(
                max_offset,
                power_management->watchdog_freq_offset_mhz + WATCHDOG_FREQ_STEP_DOWN_MHZ);
        }
        float effective_freq = calculate_effective_frequency(
            nvs_freq,
            power_management->watchdog_freq_offset_mhz + power_management->thermal_offset_mhz);
        ESP_LOGW(TAG, "ASIC Watchdog Level 2 (failure %u/3 in <10 min): RAM step-down to %.1f MHz (NVS %.1f MHz unchanged)",
                 power_management->watchdog_consecutive_failures, effective_freq, nvs_freq);
    } else {
        ESP_LOGW(TAG, "ASIC Watchdog Level 1 (failure 1/3): soft re-init of ASIC");
    }

    mining_stop(GLOBAL_STATE);
    vTaskDelay(pdMS_TO_TICKS(WATCHDOG_RECOVERY_DELAY_MS));
    mining_start(GLOBAL_STATE);
    power_management->watchdog_resets++;
}

void POWER_MANAGEMENT_init_frequency(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    float nvs_freq = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
    power_management->target_frequency = nvs_freq;

    float total_offset = power_management->watchdog_freq_offset_mhz + power_management->thermal_offset_mhz;
    float frequency = calculate_effective_frequency(nvs_freq, total_offset);

    power_management->frequency_value = frequency;
    power_management->actual_frequency = 50.0;
    power_management->expected_hashrate = expected_hashrate(GLOBAL_STATE);
    power_management->last_freq_change_us = esp_timer_get_time();

    char expected_hashrate_str[16] = {0};
    suffixString(power_management->expected_hashrate * 1e6, expected_hashrate_str, sizeof(expected_hashrate_str), 0);
    ESP_LOGI(TAG, "ASIC Frequency: %g MHz, Expected hashrate: %sH/s", frequency, expected_hashrate_str);
}

static void clamp_thermal_offsets_to_headroom(PowerManagementModule * power_management)
{
    uint16_t clamped_target_vcore = clamp_vcore_mv(power_management->target_voltage);
    uint16_t max_vcore_offset = (clamped_target_vcore > VCORE_MIN_MV) ? (clamped_target_vcore - VCORE_MIN_MV) : 0;
    if (power_management->thermal_offset_mv > max_vcore_offset) {
        power_management->thermal_offset_mv = max_vcore_offset;
    }

    float max_freq_offset = fmaxf(0.0f, power_management->target_frequency - power_management->watchdog_freq_offset_mhz - FREQ_MIN_MHZ);
    if (power_management->thermal_offset_mhz > max_freq_offset) {
        power_management->thermal_offset_mhz = max_freq_offset;
    }
}

static void apply_effective_voltage(GlobalState * GLOBAL_STATE, uint16_t effective_voltage_mv)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    if (effective_voltage_mv == power_management->actual_voltage_mv) {
        return;
    }

    ESP_LOGI(TAG, "Setting VCore to %umV (target: %umV, thermal offset: -%umV)",
             effective_voltage_mv, power_management->target_voltage, power_management->thermal_offset_mv);
    VCORE_set_voltage(GLOBAL_STATE, (double) effective_voltage_mv / 1000.0);
    power_management->actual_voltage_mv = effective_voltage_mv;
}

static void apply_effective_frequency(GlobalState * GLOBAL_STATE, float effective_freq_mhz)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    if (fabsf(effective_freq_mhz - power_management->frequency_value) <= 0.001f) {
        return;
    }

    power_management->frequency_value = effective_freq_mhz;
    power_management->expected_hashrate = expected_hashrate(GLOBAL_STATE);

    if (GLOBAL_STATE->ASIC_initalized) {
        ESP_LOGI(TAG, "Setting ASIC frequency to %g MHz (target: %g MHz, thermal: -%g MHz, watchdog: -%g MHz)",
                 effective_freq_mhz, power_management->target_frequency,
                 power_management->thermal_offset_mhz, power_management->watchdog_freq_offset_mhz);
        ASIC_set_frequency(GLOBAL_STATE);
        ASIC_set_nonce_space(GLOBAL_STATE);
        power_management->last_freq_change_us = esp_timer_get_time();
    }
}

static void apply_effective_power_and_freq(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    uint16_t effective_voltage_mv = calculate_effective_voltage(power_management->target_voltage,
                                                                power_management->thermal_offset_mv);
    float total_freq_offset_mhz = power_management->watchdog_freq_offset_mhz + power_management->thermal_offset_mhz;
    float effective_freq_mhz = calculate_effective_frequency(power_management->target_frequency,
                                                             total_freq_offset_mhz);

    // Lower frequency before dropping VCore when stepping down; raise VCore before raising frequency.
    if (effective_freq_mhz < power_management->frequency_value) {
        apply_effective_frequency(GLOBAL_STATE, effective_freq_mhz);
        apply_effective_voltage(GLOBAL_STATE, effective_voltage_mv);
    } else {
        apply_effective_voltage(GLOBAL_STATE, effective_voltage_mv);
        apply_effective_frequency(GLOBAL_STATE, effective_freq_mhz);
    }
}

static void handle_thermal_state_machine(GlobalState * GLOBAL_STATE, bool * prev_asic_initialized)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

    int64_t now_us = esp_timer_get_time();
    float chip1 = power_management->chip_temp_avg;
    float chip2 = power_management->chip_temp2_avg;
    float vr = power_management->vr_temp;
    float power = power_management->power;

    bool chip1_valid = is_valid_temp(chip1);
    bool chip2_valid = is_valid_temp(chip2);
    bool vr_valid = is_valid_temp(vr);
    bool power_valid = !isnan(power);

    bool asic_emergency = (chip1_valid && chip1 >= ASIC_EMERGENCY_TEMP) ||
                          (chip2_valid && chip2 >= ASIC_EMERGENCY_TEMP);
    bool vr_emergency = vr_valid && vr >= VR_EMERGENCY_TEMP;
    bool nvs_overheat_mode = nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE);

    // 1. Critical Emergency Shutdown (ASIC >= 75 C or VR >= 105 C)
    if (asic_emergency || vr_emergency) {
        if (!sys_module->overheat_mode) {
            if (chip2_valid) {
                ESP_LOGE(TAG, "OVERHEAT! VR: %.1fC ASIC1: %.1fC ASIC2: %.1fC - entering safe mode (NVS settings preserved)",
                         vr, chip1, chip2);
            } else {
                ESP_LOGE(TAG, "OVERHEAT! VR: %.1fC ASIC: %.1fC - entering safe mode (NVS settings preserved)",
                         vr, chip1);
            }
        }
        if (GLOBAL_STATE->ASIC_initalized || power_management->actual_voltage_mv > 0) {
            mining_stop(GLOBAL_STATE);
            *prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;
        }
        power_management->actual_voltage_mv = 0;
        reset_thermal_throttle_state(power_management);
        reset_watchdog_state(power_management);

        if (!nvs_overheat_mode) {
            nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, true);
        }
        sys_module->overheat_mode = true;
        return;
    }

    // 2. Active Overheat Latch / Safe Resume Check
    if (sys_module->overheat_mode) {
        if (!nvs_overheat_mode) {
            // While ASIC is powered down, Thermal_get_chip_temp() returns -1 (invalid).
            // Check every valid sensor reading against safe recovery limits before restarting.
            bool asic_safe_for_resume = (!chip1_valid || chip1 < ASIC_SAFE_RECOVERY_TEMP) &&
                                        (!chip2_valid || chip2 < ASIC_SAFE_RECOVERY_TEMP);
            bool vr_safe_for_resume = (!vr_valid || vr < VR_SAFE_RECOVERY_TEMP);

            if (asic_safe_for_resume && vr_safe_for_resume) {
                ESP_LOGI(TAG, "Overheat mode cleared and temperatures safe (VR: %.1fC). Resuming mining at user target (%.1f MHz / %u mV)",
                         vr, power_management->target_frequency, power_management->target_voltage);
                reset_thermal_throttle_state(power_management);
                reset_watchdog_state(power_management);
                sys_module->overheat_mode = false;
                mining_start(GLOBAL_STATE);
                *prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;
            }
        } else if (GLOBAL_STATE->ASIC_initalized || power_management->actual_voltage_mv > 0) {
            mining_stop(GLOBAL_STATE);
            *prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;
        }
        return;
    }

    if (nvs_overheat_mode) {
        ESP_LOGW(TAG, "Overheat mode enabled via NVS - stopping mining");
        sys_module->overheat_mode = true;
        if (GLOBAL_STATE->ASIC_initalized || power_management->actual_voltage_mv > 0) {
            mining_stop(GLOBAL_STATE);
            *prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;
        }
        reset_thermal_throttle_state(power_management);
        return;
    }

    // 3. Soft-Throttling Zone (ASIC >= 68 C, VR >= 90 C, or Power > 32 W)
    bool asic_soft = (chip1_valid && chip1 >= ASIC_SOFT_THROTTLE_TEMP) ||
                     (chip2_valid && chip2 >= ASIC_SOFT_THROTTLE_TEMP);
    bool vr_soft = vr_valid && vr >= VR_SOFT_THROTTLE_TEMP;
    bool power_over = power_valid && power > MAX_POWER_W;

    if (asic_soft || vr_soft || power_over) {
        bool was_throttle_active = power_management->throttle_active;
        power_management->throttle_active = true;
        power_management->safe_zone_since_us = 0;

        if (!was_throttle_active || power_management->last_throttle_step_us == 0) {
            power_management->last_throttle_step_us = now_us;
            ESP_LOGW(TAG, "Soft-throttling activated (ASIC: %.1fC, VR: %.1fC, Power: %.1fW) - fan forced to 100%% in RAM",
                     chip1, vr, power);
            return;
        }

        if ((now_us - power_management->last_throttle_step_us) >= THROTTLE_STEP_INTERVAL_US) {
            bool asic_severe = (chip1_valid && chip1 >= ASIC_SEVERE_THROTTLE_TEMP) ||
                               (chip2_valid && chip2 >= ASIC_SEVERE_THROTTLE_TEMP);
            bool vr_severe = vr_valid && vr >= VR_SEVERE_THROTTLE_TEMP;
            float freq_step = (asic_severe || vr_severe) ? THROTTLE_SEVERE_FREQ_STEP_MHZ : THROTTLE_FREQ_STEP_MHZ;

            float max_freq_offset = fmaxf(0.0f, power_management->target_frequency - power_management->watchdog_freq_offset_mhz - FREQ_MIN_MHZ);
            power_management->thermal_offset_mhz = fminf(max_freq_offset, power_management->thermal_offset_mhz + freq_step);

            uint16_t clamped_target_vcore = clamp_vcore_mv(power_management->target_voltage);
            uint16_t max_vcore_offset = (clamped_target_vcore > VCORE_MIN_MV) ? (clamped_target_vcore - VCORE_MIN_MV) : 0;
            uint32_t next_vcore_offset = (uint32_t) power_management->thermal_offset_mv + THROTTLE_VCORE_STEP_MV;
            power_management->thermal_offset_mv = (next_vcore_offset > max_vcore_offset)
                                                      ? max_vcore_offset
                                                      : (uint16_t) next_vcore_offset;

            power_management->last_throttle_step_us = now_us;

            float eff_freq = calculate_effective_frequency(power_management->target_frequency,
                                                           power_management->watchdog_freq_offset_mhz + power_management->thermal_offset_mhz);
            uint16_t eff_vcore = calculate_effective_voltage(power_management->target_voltage,
                                                             power_management->thermal_offset_mv);
            ESP_LOGW(TAG, "Soft-throttling step-down (ASIC: %.1fC, VR: %.1fC, Power: %.1fW) -> %.1f MHz / %u mV",
                     chip1, vr, power, eff_freq, eff_vcore);
        }
        return;
    }

    // 4. Hysteresis Recovery Zone (ASIC < 65 C, VR < 85 C, Power <= 32 W continuously for 30 s)
    if (power_management->throttle_active) {
        bool asic_safe = (chip1_valid || chip2_valid) &&
                         (!chip1_valid || chip1 < ASIC_SAFE_RECOVERY_TEMP) &&
                         (!chip2_valid || chip2 < ASIC_SAFE_RECOVERY_TEMP);
        bool vr_safe = !isnan(vr) && (!GLOBAL_STATE->DEVICE_CONFIG.TPS546 || vr > 0.0f) && vr < VR_SAFE_RECOVERY_TEMP;
        bool power_safe = power_valid && power <= MAX_POWER_W;

        if (!(asic_safe && vr_safe && power_safe)) {
            power_management->safe_zone_since_us = 0;
            return;
        }

        if (power_management->safe_zone_since_us == 0) {
            power_management->safe_zone_since_us = now_us;
            ESP_LOGI(TAG, "Entered thermal safe zone (ASIC: %.1fC, VR: %.1fC, Power: %.1fW) - waiting 30s hysteresis",
                     chip1, vr, power);
            return;
        }

        if ((now_us - power_management->safe_zone_since_us) >= RECOVERY_HOLD_US &&
            (now_us - power_management->last_throttle_step_us) >= RECOVERY_STEP_INTERVAL_US) {
            power_management->thermal_offset_mhz = fmaxf(0.0f, power_management->thermal_offset_mhz - THROTTLE_FREQ_STEP_MHZ);
            if (power_management->thermal_offset_mv > THROTTLE_VCORE_STEP_MV) {
                power_management->thermal_offset_mv -= THROTTLE_VCORE_STEP_MV;
            } else {
                power_management->thermal_offset_mv = 0;
            }
            power_management->last_throttle_step_us = now_us;

            if (power_management->thermal_offset_mhz <= 0.0f && power_management->thermal_offset_mv == 0) {
                reset_thermal_throttle_state(power_management);
                ESP_LOGI(TAG, "Thermal throttling cleared: restored to target %.1f MHz / %u mV",
                         power_management->target_frequency, power_management->target_voltage);
            } else {
                float eff_freq = calculate_effective_frequency(power_management->target_frequency,
                                                               power_management->watchdog_freq_offset_mhz + power_management->thermal_offset_mhz);
                uint16_t eff_vcore = calculate_effective_voltage(power_management->target_voltage,
                                                                 power_management->thermal_offset_mv);
                ESP_LOGI(TAG, "Thermal recovery step-up -> %.1f MHz / %u mV (offsets: -%.1f MHz / -%u mV)",
                         eff_freq, eff_vcore, power_management->thermal_offset_mhz, power_management->thermal_offset_mv);
            }
        }
    }
}

void POWER_MANAGEMENT_task(void * pvParameters)
{
    ESP_LOGI(TAG, "Starting");

    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

    power_management->target_frequency = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
    power_management->target_voltage = GLOBAL_STATE->SELF_TEST_MODULE.is_active
                                           ? GLOBAL_STATE->DEVICE_CONFIG.family.asic.default_voltage_mv
                                           : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
    power_management->actual_voltage_mv = 0;
    reset_thermal_throttle_state(power_management);

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);

    float last_nvs_asic_frequency = power_management->target_frequency;
    bool prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;

    vTaskDelay(500 / portTICK_PERIOD_MS);
    bool is_paused = false;

    while (1) {
        if (GLOBAL_STATE->SELF_TEST_MODULE.is_finished) {
            ESP_LOGI(TAG, "Stopped");
            vTaskDelete(NULL);
            return;
        }

        bool cur_asic_initialized = GLOBAL_STATE->ASIC_initalized;
        if (!prev_asic_initialized && cur_asic_initialized) {
            power_management->last_freq_change_us = esp_timer_get_time();
        }
        prev_asic_initialized = cur_asic_initialized;

        power_management->voltage = Power_get_input_voltage(GLOBAL_STATE);
        Power_get_output(GLOBAL_STATE, &power_management->power, &power_management->current);
        power_management->core_voltage = VCORE_get_voltage_mv(GLOBAL_STATE);

        power_management->chip_temp_avg = Thermal_get_chip_temp(GLOBAL_STATE);
        power_management->chip_temp2_avg = Thermal_get_chip_temp2(GLOBAL_STATE);

        power_management->vr_temp = Power_get_vreg_temp(GLOBAL_STATE);

        uint16_t raw_target_voltage = GLOBAL_STATE->SELF_TEST_MODULE.is_active
                                          ? GLOBAL_STATE->DEVICE_CONFIG.family.asic.default_voltage_mv
                                          : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE);
        float raw_target_frequency = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
        power_management->target_voltage = raw_target_voltage;
        power_management->target_frequency = raw_target_frequency;

        if (fabsf(raw_target_frequency - last_nvs_asic_frequency) > 0.001f) {
            ESP_LOGI(TAG, "New ASIC target frequency requested: %g MHz (previous NVS: %g MHz)",
                     raw_target_frequency, last_nvs_asic_frequency);
            reset_watchdog_state(power_management);
            last_nvs_asic_frequency = raw_target_frequency;
        }
        clamp_thermal_offsets_to_headroom(power_management);

        // User pause, hardware fault, or all pools unreachable
        bool wants_stop = sys_module->mining_paused || sys_module->hardware_fault || sys_module->pools_unavailable;
        if (wants_stop && !is_paused) {
            mining_stop(GLOBAL_STATE);
            is_paused = true;
            prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;
        } else if (!wants_stop && is_paused) {
            if (!sys_module->overheat_mode && !nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE)) {
                mining_start(GLOBAL_STATE);
                prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;
            }
            is_paused = false;
        }

        // If we've paused or have a hardware fault, skip doing anything else
        if (is_paused || sys_module->hardware_fault) {
            power_management->asic_watchdog_trigger = false;
            vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
            continue;
        }

        handle_thermal_state_machine(GLOBAL_STATE, &prev_asic_initialized);

        if (!sys_module->overheat_mode) {
            apply_effective_power_and_freq(GLOBAL_STATE);
        }

        VCORE_check_fault(GLOBAL_STATE);

        if (power_management->asic_watchdog_trigger) {
            power_management->asic_watchdog_trigger = false;

            if (!sys_module->overheat_mode && sys_module->power_fault == 0 &&
                !sys_module->hardware_fault && !sys_module->mining_paused &&
                !sys_module->pools_unavailable && !GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
                handle_asic_watchdog(GLOBAL_STATE);
                prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;
            }
        }

        // looper:
        vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
    }
}
