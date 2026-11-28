#include <math.h>
#include <stdlib.h>
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
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
#include "power_control_math.h"
#include "dvfs_bandit.h"

#define POLL_RATE 100

// Hard Caps (physical safety limits in RAM)
#define VCORE_MIN_MV PCM_VCORE_MIN_MV
#define VCORE_MAX_MV PCM_VCORE_MAX_MV
#define FREQ_MIN_MHZ PCM_FREQ_MIN_MHZ
#define MAX_POWER_W  PCM_MAX_POWER_W

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

// Slow-loop Vdroop compensation
#define VDROOP_CHECK_INTERVAL_US       (10LL * 1000000LL)
#define VDROOP_FREQ_SETTLE_US          (10LL * 1000000LL)
#define VDROOP_STEP_MV                 PCM_VDROOP_STEP_MV
#define VDROOP_MAX_OFFSET_MV           PCM_VDROOP_MAX_OFFSET_MV
#define VDROOP_DEADBAND_MV             PCM_VDROOP_DEADBAND_MV
#define VDROOP_VALID_VOLTAGE_MIN_MV    PCM_VDROOP_VALID_VOLTAGE_MIN_MV

// DVFS profiles and Auto-Tuner envelopes
#define DVFS_STEP_INTERVAL_US          (60LL * 1000000LL)

typedef enum {
    DVFS_PROFILE_MANUAL = PCM_DVFS_PROFILE_MANUAL,
    DVFS_PROFILE_EFFICIENCY = PCM_DVFS_PROFILE_EFFICIENCY,
    DVFS_PROFILE_BALANCED = PCM_DVFS_PROFILE_BALANCED,
    DVFS_PROFILE_PERFORMANCE = PCM_DVFS_PROFILE_PERFORMANCE,
    DVFS_PROFILE_ADAPTIVE = PCM_DVFS_PROFILE_ADAPTIVE,
} DvfsProfile;

static const char * TAG = "power_management";

static RTC_NOINIT_ATTR PcmRtcDiagnostics s_rtc_diag;
static DvfsBanditState * s_bandit_state = NULL;

static DvfsBanditState * ensure_bandit_state_allocated(void)
{
    if (s_bandit_state != NULL) {
        return s_bandit_state;
    }
    if (esp_psram_is_initialized()) {
        s_bandit_state = heap_caps_malloc(sizeof(DvfsBanditState), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_bandit_state == NULL) {
        s_bandit_state = malloc(sizeof(DvfsBanditState));
    }
    return s_bandit_state;
}

static void sync_rtc_diagnostics(const PowerManagementModule * power_management)
{
    s_rtc_diag.watchdog_resets = power_management->watchdog_resets;
    s_rtc_diag.cml_error_count = power_management->cml_error_count;
    s_rtc_diag.last_watchdog_reason = power_management->last_watchdog_reason;
    s_rtc_diag.learned_max_freq_mhz = power_management->learned_max_freq_mhz;
    s_rtc_diag.learned_min_vcore_mv = power_management->learned_min_vcore_mv;
    s_rtc_diag.adaptive_best_freq_mhz = power_management->adaptive_best_freq_mhz;
    s_rtc_diag.adaptive_best_vcore_mv = power_management->adaptive_best_vcore_mv;
    pcm_rtc_diag_seal(&s_rtc_diag,
                      power_management->target_frequency,
                      power_management->target_voltage,
                      power_management->dvfs_profile);
}

static bool is_valid_temp(float temp)
{
    return !isnan(temp) && temp > 0.0f;
}

static bool is_vr_temp_valid(const GlobalState * GLOBAL_STATE, float vr)
{
    return !isnan(vr) && (!GLOBAL_STATE->DEVICE_CONFIG.TPS546 || vr > 0.0f);
}

static float get_max_valid_chip_temp(float chip1, float chip2)
{
    float max_temp = 0.0f;
    if (is_valid_temp(chip1)) {
        max_temp = chip1;
    }
    if (is_valid_temp(chip2) && chip2 > max_temp) {
        max_temp = chip2;
    }
    return max_temp;
}

static bool are_asic_temps_below(float chip1, float chip2, float limit)
{
    bool chip1_valid = is_valid_temp(chip1);
    bool chip2_valid = is_valid_temp(chip2);
    return (chip1_valid || chip2_valid) &&
           (!chip1_valid || chip1 < limit) &&
           (!chip2_valid || chip2 < limit);
}

static float get_dvfs_base_target_freq(const PowerManagementModule * power_management)
{
    return pcm_compute_dvfs_base_freq(power_management->dvfs_profile,
                                      power_management->target_frequency,
                                      power_management->dvfs_freq_offset_mhz);
}

static uint16_t get_dvfs_base_target_voltage_mv(const PowerManagementModule * power_management)
{
    return pcm_compute_effective_vcore(power_management->target_voltage,
                                       power_management->dvfs_voltage_offset_mv,
                                       0,
                                       0);
}

static uint16_t get_base_voltage_mv(const PowerManagementModule * power_management)
{
    return pcm_compute_effective_vcore(power_management->target_voltage,
                                       power_management->dvfs_voltage_offset_mv,
                                       power_management->thermal_offset_mv,
                                       0);
}

static uint16_t get_effective_voltage_mv(const PowerManagementModule * power_management)
{
    return pcm_compute_effective_vcore(power_management->target_voltage,
                                       power_management->dvfs_voltage_offset_mv,
                                       power_management->thermal_offset_mv,
                                       power_management->vdroop_offset_mv);
}

static float get_effective_frequency_mhz(const PowerManagementModule * power_management)
{
    return pcm_compute_effective_freq(power_management->dvfs_profile,
                                      power_management->target_frequency,
                                      power_management->dvfs_freq_offset_mhz,
                                      power_management->thermal_offset_mhz,
                                      power_management->watchdog_freq_offset_mhz);
}

static void reset_dvfs_state(PowerManagementModule * power_management, int64_t now_us)
{
    power_management->dvfs_freq_offset_mhz = 0.0f;
    power_management->dvfs_voltage_offset_mv = 0;
    power_management->learned_max_freq_mhz = 0.0f;
    power_management->learned_min_vcore_mv = 0;
    power_management->adaptive_best_freq_mhz = 0.0f;
    power_management->adaptive_best_vcore_mv = 0;
    power_management->last_dvfs_step_us = now_us;
    if (s_bandit_state != NULL) {
        dvfs_bandit_init(s_bandit_state,
                         power_management->target_frequency,
                         power_management->target_voltage,
                         0.0f,
                         0);
    }
}

static void reset_vdroop_state(PowerManagementModule * power_management)
{
    power_management->vdroop_offset_mv = 0;
    power_management->last_vdroop_check_us = 0;
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

    // Mark uninitialized immediately so Core 1 tasks stop issuing UART1 commands
    // before frequency ramp-down, voltage cut, or reset assertion
    GLOBAL_STATE->ASIC_initalized = false;

    // Give in-flight UART1 operations (create_jobs_task / hashrate_monitor_task) time to drain
    vTaskDelay(pdMS_TO_TICKS(200));

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
    reset_vdroop_state(&GLOBAL_STATE->POWER_MANAGEMENT_MODULE);
    asic_hold_reset_low();

    // Give tasks time to settle after ASIC shutdown
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

    uint16_t effective_voltage_mv = get_effective_voltage_mv(power_management);
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
        int64_t now_us = esp_timer_get_time();
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.last_freq_change_us = now_us;
        GLOBAL_STATE->POWER_MANAGEMENT_MODULE.last_dvfs_step_us = now_us;
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
    power_management->pre_watchdog_trigger = false;
}

static void handle_asic_watchdog(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    int64_t now_us = esp_timer_get_time();

    if (power_management->watchdog_trigger_reason != PCM_WATCHDOG_REASON_NONE) {
        power_management->last_watchdog_reason = power_management->watchdog_trigger_reason;
        power_management->watchdog_trigger_reason = PCM_WATCHDOG_REASON_NONE;
    }

    if (power_management->watchdog_last_failure_us > 0 &&
        (now_us - power_management->watchdog_last_failure_us) < WATCHDOG_FAILURE_WINDOW_US) {
        power_management->watchdog_consecutive_failures++;
    } else {
        power_management->watchdog_consecutive_failures = 1;
    }
    power_management->watchdog_last_failure_us = now_us;

    if (power_management->watchdog_consecutive_failures >= 4) {
        power_management->watchdog_resets++;
        power_management->last_watchdog_reason = PCM_WATCHDOG_REASON_LEVEL3_REBOOT;
        sync_rtc_diagnostics(power_management);
        ESP_LOGE(TAG, "ASIC Watchdog Level 3 (%u consecutive failures in <10 min) - restarting system",
                 power_management->watchdog_consecutive_failures);
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;
    }

    if (power_management->dvfs_profile == DVFS_PROFILE_ADAPTIVE) {
        DvfsBanditState * bandit = ensure_bandit_state_allocated();
        if (bandit != NULL) {
            float rb_freq = get_dvfs_base_target_freq(power_management);
            uint16_t rb_vcore = get_dvfs_base_target_voltage_mv(power_management);
            dvfs_bandit_record_fault_and_rollback(bandit, true, &rb_freq, &rb_vcore);
            power_management->dvfs_freq_offset_mhz = rb_freq - power_management->target_frequency;
            power_management->dvfs_voltage_offset_mv = (int16_t) ((int32_t) rb_vcore - (int32_t) power_management->target_voltage);
            power_management->adaptive_best_freq_mhz = bandit->best_safe_freq_mhz;
            power_management->adaptive_best_vcore_mv = bandit->best_safe_vcore_mv;
            if (power_management->watchdog_consecutive_failures >= 2 && rb_freq <= (DVFS_BANDIT_FREQ_MIN_MHZ + 0.001f)) {
                pcm_dvfs_apply_watchdog_event(PCM_DVFS_PROFILE_MANUAL,
                                              power_management->watchdog_consecutive_failures,
                                              rb_freq,
                                              rb_vcore,
                                              NULL,
                                              NULL,
                                              &power_management->watchdog_freq_offset_mhz,
                                              NULL,
                                              NULL);
            }
        } else {
            pcm_dvfs_apply_watchdog_event(power_management->dvfs_profile,
                                          power_management->watchdog_consecutive_failures,
                                          power_management->target_frequency,
                                          power_management->target_voltage,
                                          &power_management->dvfs_freq_offset_mhz,
                                          &power_management->dvfs_voltage_offset_mv,
                                          &power_management->watchdog_freq_offset_mhz,
                                          &power_management->learned_max_freq_mhz,
                                          &power_management->learned_min_vcore_mv);
        }
    } else {
        pcm_dvfs_apply_watchdog_event(power_management->dvfs_profile,
                                      power_management->watchdog_consecutive_failures,
                                      power_management->target_frequency,
                                      power_management->target_voltage,
                                      &power_management->dvfs_freq_offset_mhz,
                                      &power_management->dvfs_voltage_offset_mv,
                                      &power_management->watchdog_freq_offset_mhz,
                                      &power_management->learned_max_freq_mhz,
                                      &power_management->learned_min_vcore_mv);
    }

    float effective_freq = get_effective_frequency_mhz(power_management);
    uint16_t effective_vcore = get_effective_voltage_mv(power_management);

    if (power_management->watchdog_consecutive_failures >= 2) {
        ESP_LOGW(TAG,
                 "ASIC Watchdog Level 2 (failure %u/3 in <10 min): RAM step-down to %.1f MHz / %u mV (NVS %.1f MHz unchanged, learned max: %.1f MHz, learned min VCore: %u mV)",
                 power_management->watchdog_consecutive_failures,
                 effective_freq,
                 effective_vcore,
                 power_management->target_frequency,
                 power_management->learned_max_freq_mhz,
                 power_management->learned_min_vcore_mv);
    } else {
        ESP_LOGW(TAG,
                 "ASIC Watchdog Level 1 (failure 1/3): soft re-init of ASIC at %.1f MHz / %u mV (learned max: %.1f MHz)",
                 effective_freq,
                 effective_vcore,
                 power_management->learned_max_freq_mhz);
    }

    mining_stop(GLOBAL_STATE);
    vTaskDelay(pdMS_TO_TICKS(WATCHDOG_RECOVERY_DELAY_MS));
    mining_start(GLOBAL_STATE);
    power_management->watchdog_resets++;
    sync_rtc_diagnostics(power_management);
}

void POWER_MANAGEMENT_init_frequency(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    float nvs_freq = nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY);
    uint16_t dvfs_profile = GLOBAL_STATE->SELF_TEST_MODULE.is_active
                                ? DVFS_PROFILE_MANUAL
                                : nvs_config_get_u16(NVS_CONFIG_DVFS_PROFILE);
    if (dvfs_profile > DVFS_PROFILE_ADAPTIVE) {
        dvfs_profile = DVFS_PROFILE_MANUAL;
    }
    power_management->target_frequency = nvs_freq;
    power_management->dvfs_profile = dvfs_profile;

    float frequency = get_effective_frequency_mhz(power_management);

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
    uint16_t clamped_target_vcore = get_dvfs_base_target_voltage_mv(power_management);
    uint16_t max_vcore_offset = (clamped_target_vcore > VCORE_MIN_MV) ? (clamped_target_vcore - VCORE_MIN_MV) : 0;
    if (power_management->thermal_offset_mv > max_vcore_offset) {
        power_management->thermal_offset_mv = max_vcore_offset;
    }

    float base_target_freq = get_dvfs_base_target_freq(power_management);
    float max_freq_offset = fmaxf(0.0f, base_target_freq - power_management->watchdog_freq_offset_mhz - FREQ_MIN_MHZ);
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

    ESP_LOGI(TAG, "Setting VCore to %umV (target: %umV, dvfs: %+dmV, thermal: -%umV, vdroop: +%umV)",
             effective_voltage_mv, power_management->target_voltage, power_management->dvfs_voltage_offset_mv,
             power_management->thermal_offset_mv, power_management->vdroop_offset_mv);
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
        ESP_LOGI(TAG, "Setting ASIC frequency to %g MHz (target: %g MHz, dvfs: %+g MHz, thermal: -%g MHz, watchdog: -%g MHz)",
                 effective_freq_mhz, power_management->target_frequency, power_management->dvfs_freq_offset_mhz,
                 power_management->thermal_offset_mhz, power_management->watchdog_freq_offset_mhz);
        ASIC_set_frequency(GLOBAL_STATE);
        ASIC_set_nonce_space(GLOBAL_STATE);
        power_management->last_freq_change_us = esp_timer_get_time();
    }
}

static void apply_effective_power_and_freq(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;

    uint16_t effective_voltage_mv = get_effective_voltage_mv(power_management);
    float effective_freq_mhz = get_effective_frequency_mhz(power_management);

    // Lower frequency before dropping VCore when stepping down; raise VCore before raising frequency.
    if (effective_freq_mhz < power_management->frequency_value) {
        apply_effective_frequency(GLOBAL_STATE, effective_freq_mhz);
        apply_effective_voltage(GLOBAL_STATE, effective_voltage_mv);
    } else {
        apply_effective_voltage(GLOBAL_STATE, effective_voltage_mv);
        apply_effective_frequency(GLOBAL_STATE, effective_freq_mhz);
    }
}

static void handle_dvfs_autotuner(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;
    int64_t now_us = esp_timer_get_time();

    if (power_management->dvfs_profile == DVFS_PROFILE_MANUAL || GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
        reset_dvfs_state(power_management, now_us);
        return;
    }

    if (!GLOBAL_STATE->ASIC_initalized || sys_module->overheat_mode || power_management->throttle_active) {
        power_management->last_dvfs_step_us = now_us;
        return;
    }

    if (power_management->last_dvfs_step_us <= 0) {
        power_management->last_dvfs_step_us = now_us;
        return;
    }

    if ((now_us - power_management->last_dvfs_step_us) < DVFS_STEP_INTERVAL_US) {
        return;
    }
    power_management->last_dvfs_step_us = now_us;

    float err_pct = isnan(sys_module->error_percentage) ? 0.0f : sys_module->error_percentage;
    float chip_temp = get_max_valid_chip_temp(power_management->chip_temp_avg, power_management->chip_temp2_avg);
    float vr_for_dvfs = is_vr_temp_valid(GLOBAL_STATE, power_management->vr_temp)
                            ? fmaxf(0.0f, power_management->vr_temp)
                            : NAN;
    float power = power_management->power;

    float prev_freq = get_dvfs_base_target_freq(power_management);
    uint16_t prev_mv = get_dvfs_base_target_voltage_mv(power_management);
    float prev_learned_freq = power_management->learned_max_freq_mhz;
    uint16_t prev_learned_mv = power_management->learned_min_vcore_mv;

    switch (power_management->dvfs_profile) {
        case DVFS_PROFILE_EFFICIENCY:
            pcm_dvfs_step_efficiency(power_management->target_voltage,
                                     &power_management->dvfs_voltage_offset_mv,
                                     &power_management->learned_min_vcore_mv,
                                     err_pct,
                                     power);
            break;
        case DVFS_PROFILE_BALANCED:
            pcm_dvfs_step_balanced(power_management->target_frequency,
                                   &power_management->dvfs_freq_offset_mhz,
                                   &power_management->dvfs_voltage_offset_mv,
                                   &power_management->learned_max_freq_mhz,
                                   err_pct,
                                   chip_temp,
                                   vr_for_dvfs,
                                   power);
            break;
        case DVFS_PROFILE_PERFORMANCE:
            pcm_dvfs_step_performance(power_management->target_frequency,
                                      power_management->target_voltage,
                                      &power_management->dvfs_freq_offset_mhz,
                                      &power_management->dvfs_voltage_offset_mv,
                                      &power_management->learned_max_freq_mhz,
                                      &power_management->learned_min_vcore_mv,
                                      err_pct,
                                      chip_temp,
                                      vr_for_dvfs,
                                      power);
            break;
        case DVFS_PROFILE_ADAPTIVE: {
            DvfsBanditState * bandit = ensure_bandit_state_allocated();
            if (bandit != NULL) {
                float prev_best_freq = power_management->adaptive_best_freq_mhz;
                uint16_t prev_best_mv = power_management->adaptive_best_vcore_mv;
                float next_bandit_freq = get_dvfs_base_target_freq(power_management);
                uint16_t next_bandit_mv = get_dvfs_base_target_voltage_mv(power_management);
                float active_hr = (sys_module->hashrate_1m > 0.0f) ? sys_module->hashrate_1m : sys_module->current_hashrate;
                DvfsBanditSample sample = {
                    .hashrate_ghs = active_hr,
                    .expected_hashrate_ghs = power_management->expected_hashrate,
                    .error_percentage = err_pct,
                    .power_w = power,
                    .chip_temp_c = chip_temp,
                    .vr_temp_c = vr_for_dvfs,
                };
                dvfs_bandit_step(bandit, &sample,
                                 &next_bandit_freq, &next_bandit_mv,
                                 &power_management->adaptive_best_freq_mhz,
                                 &power_management->adaptive_best_vcore_mv);
                power_management->dvfs_freq_offset_mhz = next_bandit_freq - power_management->target_frequency;
                power_management->dvfs_voltage_offset_mv = (int16_t) ((int32_t) next_bandit_mv - (int32_t) power_management->target_voltage);
                if (power_management->adaptive_best_vcore_mv != prev_best_mv ||
                    fabsf(power_management->adaptive_best_freq_mhz - prev_best_freq) > 0.001f) {
                    ESP_LOGI(TAG, "Adaptive 2D Bandit updated safe point -> %.1f MHz / %u mV",
                             power_management->adaptive_best_freq_mhz,
                             power_management->adaptive_best_vcore_mv);
                    sync_rtc_diagnostics(power_management);
                }
            }
            break;
        }
        default:
            break;
    }

    float next_freq = get_dvfs_base_target_freq(power_management);
    uint16_t next_mv = get_dvfs_base_target_voltage_mv(power_management);

    if (next_mv != prev_mv) {
        ESP_LOGI(TAG, "DVFS profile %u VCore step -> %u mV (offset: %+d mV, learned floor: %u mV)",
                 power_management->dvfs_profile, next_mv,
                 power_management->dvfs_voltage_offset_mv, power_management->learned_min_vcore_mv);
    } else if (power_management->learned_min_vcore_mv != prev_learned_mv) {
        ESP_LOGI(TAG, "DVFS profile %u locked learned VCore floor -> %u mV",
                 power_management->dvfs_profile, power_management->learned_min_vcore_mv);
    }

    if (fabsf(next_freq - prev_freq) > 0.001f) {
        ESP_LOGI(TAG, "DVFS profile %u frequency step -> %.1f MHz (offset: %+.1f MHz, learned ceiling: %.1f MHz)",
                 power_management->dvfs_profile, next_freq,
                 power_management->dvfs_freq_offset_mhz, power_management->learned_max_freq_mhz);
    } else if (fabsf(power_management->learned_max_freq_mhz - prev_learned_freq) > 0.001f) {
        ESP_LOGI(TAG, "DVFS profile %u locked learned max frequency -> %.1f MHz",
                 power_management->dvfs_profile, power_management->learned_max_freq_mhz);
    }

    if (power_management->learned_min_vcore_mv != prev_learned_mv ||
        fabsf(power_management->learned_max_freq_mhz - prev_learned_freq) > 0.001f) {
        sync_rtc_diagnostics(power_management);
    }
}

static void handle_vdroop_compensation(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

    if (!GLOBAL_STATE->ASIC_initalized || GLOBAL_STATE->SELF_TEST_MODULE.is_active ||
        sys_module->overheat_mode || power_management->throttle_active) {
        return;
    }

    if (isnan(power_management->core_voltage) ||
        power_management->core_voltage <= VDROOP_VALID_VOLTAGE_MIN_MV) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (power_management->last_freq_change_us <= 0 ||
        (now_us - power_management->last_freq_change_us) < VDROOP_FREQ_SETTLE_US) {
        return;
    }

    if (power_management->last_vdroop_check_us > 0 &&
        (now_us - power_management->last_vdroop_check_us) < VDROOP_CHECK_INTERVAL_US) {
        return;
    }
    power_management->last_vdroop_check_us = now_us;

    uint16_t base_voltage_mv = get_base_voltage_mv(power_management);
    uint16_t prev_offset = power_management->vdroop_offset_mv;
    uint16_t next_offset = pcm_step_vdroop_offset_mv(prev_offset,
                                                     base_voltage_mv,
                                                     power_management->core_voltage,
                                                     power_management->power);
    power_management->vdroop_offset_mv = next_offset;

    if (next_offset > prev_offset) {
        ESP_LOGI(TAG, "Vdroop compensation +%u mV -> offset: +%u mV (measured: %.1f mV, base: %u mV)",
                 next_offset - prev_offset, next_offset,
                 power_management->core_voltage, base_voltage_mv);
    } else if (next_offset < prev_offset &&
               power_management->core_voltage > ((float) base_voltage_mv + VDROOP_DEADBAND_MV)) {
        ESP_LOGI(TAG, "Vdroop compensation -%u mV -> offset: +%u mV (measured: %.1f mV, base: %u mV)",
                 prev_offset - next_offset, next_offset,
                 power_management->core_voltage, base_voltage_mv);
    }
}

static void handle_pre_watchdog(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

    if (!power_management->pre_watchdog_trigger) {
        return;
    }
    power_management->pre_watchdog_trigger = false;

    if (!GLOBAL_STATE->ASIC_initalized || GLOBAL_STATE->SELF_TEST_MODULE.is_active ||
        sys_module->overheat_mode || power_management->throttle_active ||
        sys_module->power_fault != 0 || sys_module->hardware_fault ||
        sys_module->mining_paused || sys_module->pools_unavailable) {
        return;
    }

    int64_t now_us = esp_timer_get_time();
    if (power_management->last_pre_watchdog_action_us > 0 &&
        (now_us - power_management->last_pre_watchdog_action_us) < PCM_PRE_WATCHDOG_COOLDOWN_US) {
        ESP_LOGI(TAG, "ASIC Pre-Watchdog trigger ignored during 60s cooldown (ASIC %u domain %u, CUSUM: %.2f)",
                 power_management->pre_watchdog_asic,
                 power_management->pre_watchdog_domain,
                 power_management->pre_watchdog_cusum_max);
        return;
    }

    float prev_learned_freq = power_management->learned_max_freq_mhz;
    uint16_t prev_learned_mv = power_management->learned_min_vcore_mv;
    float vr_for_relief = is_vr_temp_valid(GLOBAL_STATE, power_management->vr_temp)
                              ? fmaxf(0.0f, power_management->vr_temp)
                              : NAN;

    if (power_management->dvfs_profile == DVFS_PROFILE_ADAPTIVE) {
        DvfsBanditState * bandit = ensure_bandit_state_allocated();
        if (bandit != NULL) {
            float rb_freq = get_dvfs_base_target_freq(power_management);
            uint16_t rb_vcore = get_dvfs_base_target_voltage_mv(power_management);
            dvfs_bandit_record_fault_and_rollback(bandit, false, &rb_freq, &rb_vcore);
            power_management->dvfs_freq_offset_mhz = rb_freq - power_management->target_frequency;
            power_management->dvfs_voltage_offset_mv = (int16_t) ((int32_t) rb_vcore - (int32_t) power_management->target_voltage);
            power_management->adaptive_best_freq_mhz = bandit->best_safe_freq_mhz;
            power_management->adaptive_best_vcore_mv = bandit->best_safe_vcore_mv;
            power_management->pre_watchdog_recoveries++;
            power_management->last_pre_watchdog_action_us = now_us;
            power_management->last_dvfs_step_us = now_us;

            float effective_freq = get_effective_frequency_mhz(power_management);
            uint16_t effective_vcore = get_effective_voltage_mv(power_management);
            ESP_LOGW(TAG,
                     "ASIC Pre-Watchdog Adaptive rollback #%lu: ASIC %u domain %u (CUSUM: %.2f) -> %.1f MHz / %u mV (safe point: %.1f MHz / %u mV)",
                     (unsigned long) power_management->pre_watchdog_recoveries,
                     power_management->pre_watchdog_asic,
                     power_management->pre_watchdog_domain,
                     power_management->pre_watchdog_cusum_max,
                     effective_freq,
                     effective_vcore,
                     power_management->adaptive_best_freq_mhz,
                     power_management->adaptive_best_vcore_mv);
            sync_rtc_diagnostics(power_management);
            return;
        }
    }

    PcmPreWatchdogAction action = pcm_pre_watchdog_apply_relief(power_management->dvfs_profile,
                                                                power_management->target_frequency,
                                                                power_management->target_voltage,
                                                                power_management->vdroop_offset_mv,
                                                                power_management->power,
                                                                vr_for_relief,
                                                                &power_management->dvfs_freq_offset_mhz,
                                                                &power_management->dvfs_voltage_offset_mv,
                                                                &power_management->watchdog_freq_offset_mhz,
                                                                &power_management->learned_max_freq_mhz,
                                                                &power_management->learned_min_vcore_mv);

    if (action != PCM_PRE_WATCHDOG_ACTION_NONE) {
        power_management->pre_watchdog_recoveries++;
        power_management->last_pre_watchdog_action_us = now_us;
        power_management->last_dvfs_step_us = now_us;

        float effective_freq = get_effective_frequency_mhz(power_management);
        uint16_t effective_vcore = get_effective_voltage_mv(power_management);
        ESP_LOGW(TAG,
                 "ASIC Pre-Watchdog hot relief #%lu (%s): ASIC %u domain %u (CUSUM: %.2f) -> %.1f MHz / %u mV (learned max: %.1f MHz, learned min VCore: %u mV)",
                 (unsigned long) power_management->pre_watchdog_recoveries,
                 (action == PCM_PRE_WATCHDOG_ACTION_VCORE_STEP_UP) ? "+5 mV VCore" : "-12.5 MHz freq",
                 power_management->pre_watchdog_asic,
                 power_management->pre_watchdog_domain,
                 power_management->pre_watchdog_cusum_max,
                 effective_freq,
                 effective_vcore,
                 power_management->learned_max_freq_mhz,
                 power_management->learned_min_vcore_mv);

        if (power_management->learned_min_vcore_mv != prev_learned_mv ||
            fabsf(power_management->learned_max_freq_mhz - prev_learned_freq) > 0.001f) {
            sync_rtc_diagnostics(power_management);
        }
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
        reset_vdroop_state(power_management);
        reset_watchdog_state(power_management);

        if (!nvs_overheat_mode) {
            nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, true);
        }
        sys_module->overheat_mode = true;
        return;
    }

    // 2. Active Overheat Latch / Safe Resume Check
    if (sys_module->overheat_mode) {
        power_management->pre_watchdog_trigger = false;
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
        power_management->pre_watchdog_trigger = false;
        if (GLOBAL_STATE->ASIC_initalized || power_management->actual_voltage_mv > 0) {
            mining_stop(GLOBAL_STATE);
            *prev_asic_initialized = GLOBAL_STATE->ASIC_initalized;
        }
        reset_thermal_throttle_state(power_management);
        return;
    }

    // 3. Soft-Throttling Zone (ASIC >= 68 C, VR >= 90 C, Power > 32 W, or Fan Stall)
    bool asic_soft = (chip1_valid && chip1 >= ASIC_SOFT_THROTTLE_TEMP) ||
                     (chip2_valid && chip2 >= ASIC_SOFT_THROTTLE_TEMP);
    bool vr_soft = vr_valid && vr >= VR_SOFT_THROTTLE_TEMP;
    bool power_over = power_valid && power > MAX_POWER_W;
    bool fan_stalled = power_management->fan_stall;

    if (asic_soft || vr_soft || power_over || fan_stalled) {
        bool was_throttle_active = power_management->throttle_active;
        power_management->throttle_active = true;
        power_management->safe_zone_since_us = 0;

        if (!was_throttle_active || power_management->last_throttle_step_us == 0) {
            if (!was_throttle_active) {
                reset_vdroop_state(power_management);
            }
            power_management->last_throttle_step_us = now_us;
            ESP_LOGW(TAG, "Soft-throttling activated (ASIC: %.1fC, VR: %.1fC, Power: %.1fW, FanStall: %d) - fan forced to 100%% in RAM",
                     chip1, vr, power, fan_stalled ? 1 : 0);
            return;
        }

        if ((now_us - power_management->last_throttle_step_us) >= THROTTLE_STEP_INTERVAL_US) {
            bool asic_severe = (chip1_valid && chip1 >= ASIC_SEVERE_THROTTLE_TEMP) ||
                               (chip2_valid && chip2 >= ASIC_SEVERE_THROTTLE_TEMP);
            bool vr_severe = vr_valid && vr >= VR_SEVERE_THROTTLE_TEMP;
            float freq_step = (asic_severe || vr_severe) ? THROTTLE_SEVERE_FREQ_STEP_MHZ : THROTTLE_FREQ_STEP_MHZ;

            float max_freq_offset = fmaxf(0.0f, get_dvfs_base_target_freq(power_management) -
                                                power_management->watchdog_freq_offset_mhz - FREQ_MIN_MHZ);
            power_management->thermal_offset_mhz = fminf(max_freq_offset, power_management->thermal_offset_mhz + freq_step);

            uint16_t clamped_target_vcore = get_dvfs_base_target_voltage_mv(power_management);
            uint16_t max_vcore_offset = (clamped_target_vcore > VCORE_MIN_MV) ? (clamped_target_vcore - VCORE_MIN_MV) : 0;
            uint32_t next_vcore_offset = (uint32_t) power_management->thermal_offset_mv + THROTTLE_VCORE_STEP_MV;
            power_management->thermal_offset_mv = (next_vcore_offset > max_vcore_offset)
                                                      ? max_vcore_offset
                                                      : (uint16_t) next_vcore_offset;

            power_management->last_throttle_step_us = now_us;

            float eff_freq = get_effective_frequency_mhz(power_management);
            uint16_t eff_vcore = get_effective_voltage_mv(power_management);
            ESP_LOGW(TAG, "Soft-throttling step-down (ASIC: %.1fC, VR: %.1fC, Power: %.1fW, FanStall: %d) -> %.1f MHz / %u mV",
                     chip1, vr, power, fan_stalled ? 1 : 0, eff_freq, eff_vcore);
        }
        return;
    }

    // 4. Hysteresis Recovery Zone (ASIC < 65 C, VR < 85 C, Power <= 32 W, no Fan Stall continuously for 30 s)
    if (power_management->throttle_active) {
        bool asic_safe = are_asic_temps_below(chip1, chip2, ASIC_SAFE_RECOVERY_TEMP);
        bool vr_safe = is_vr_temp_valid(GLOBAL_STATE, vr) && vr < VR_SAFE_RECOVERY_TEMP;
        bool power_safe = power_valid && power <= MAX_POWER_W;
        bool fan_safe = !power_management->fan_stall;

        if (!(asic_safe && vr_safe && power_safe && fan_safe)) {
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
                         get_dvfs_base_target_freq(power_management), get_dvfs_base_target_voltage_mv(power_management));
            } else {
                float eff_freq = get_effective_frequency_mhz(power_management);
                uint16_t eff_vcore = get_effective_voltage_mv(power_management);
                ESP_LOGI(TAG, "Thermal recovery step-up -> %.1f MHz / %u mV (offsets: -%.1f MHz / -%u mV)",
                         eff_freq, eff_vcore, power_management->thermal_offset_mhz, power_management->thermal_offset_mv);
            }
        }
    }
}

static void handle_vrm_hiccup_protection(GlobalState * GLOBAL_STATE)
{
    PowerManagementModule * power_management = &GLOBAL_STATE->POWER_MANAGEMENT_MODULE;
    SystemModule * sys_module = &GLOBAL_STATE->SYSTEM_MODULE;

    if (!power_management->vrm_hiccup_stepdown_pending) {
        return;
    }
    power_management->vrm_hiccup_stepdown_pending = false;

    if (!GLOBAL_STATE->ASIC_initalized || sys_module->overheat_mode ||
        sys_module->hardware_fault || sys_module->mining_paused ||
        GLOBAL_STATE->SELF_TEST_MODULE.is_active) {
        return;
    }

    if (power_management->dvfs_profile == DVFS_PROFILE_ADAPTIVE) {
        DvfsBanditState * bandit = ensure_bandit_state_allocated();
        if (bandit != NULL) {
            float prev_eff_freq = get_effective_frequency_mhz(power_management);
            float rb_freq = get_dvfs_base_target_freq(power_management);
            uint16_t rb_vcore = get_dvfs_base_target_voltage_mv(power_management);
            dvfs_bandit_record_fault_and_rollback(bandit, true, &rb_freq, &rb_vcore);
            power_management->dvfs_freq_offset_mhz = rb_freq - power_management->target_frequency;
            power_management->dvfs_voltage_offset_mv = (int16_t) ((int32_t) rb_vcore - (int32_t) power_management->target_voltage);
            power_management->adaptive_best_freq_mhz = bandit->best_safe_freq_mhz;
            power_management->adaptive_best_vcore_mv = bandit->best_safe_vcore_mv;

            float desired_freq = fmaxf(FREQ_MIN_MHZ, prev_eff_freq - PCM_VRM_HICCUP_FREQ_STEP_MHZ);
            float cur_eff_freq = get_effective_frequency_mhz(power_management);
            if (cur_eff_freq > (desired_freq + 0.001f)) {
                float cur_base = get_dvfs_base_target_freq(power_management);
                float max_wd = fmaxf(0.0f, cur_base - FREQ_MIN_MHZ);
                power_management->watchdog_freq_offset_mhz = fminf(
                    max_wd, power_management->watchdog_freq_offset_mhz + (cur_eff_freq - desired_freq));
            }
        } else {
            pcm_dvfs_apply_vrm_hiccup_stepdown(power_management->dvfs_profile,
                                               power_management->target_frequency,
                                               &power_management->dvfs_freq_offset_mhz,
                                               &power_management->watchdog_freq_offset_mhz,
                                               &power_management->learned_max_freq_mhz);
        }
    } else {
        pcm_dvfs_apply_vrm_hiccup_stepdown(power_management->dvfs_profile,
                                           power_management->target_frequency,
                                           &power_management->dvfs_freq_offset_mhz,
                                           &power_management->watchdog_freq_offset_mhz,
                                           &power_management->learned_max_freq_mhz);
    }

    power_management->last_dvfs_step_us = esp_timer_get_time();
    apply_effective_power_and_freq(GLOBAL_STATE);
    sync_rtc_diagnostics(power_management);

    float effective_freq = get_effective_frequency_mhz(power_management);
    ESP_LOGW(TAG,
             "VRM hiccup protection step-down #%lu (STATUS_WORD=0x%04X): -> %.1f MHz (learned ceiling: %.1f MHz)",
             (unsigned long) power_management->vrm_hiccup_count,
             (unsigned int) power_management->last_vrm_hiccup_status,
             effective_freq,
             power_management->learned_max_freq_mhz);
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
    power_management->fan_stall = false;
    power_management->pre_watchdog_asic = 0;
    power_management->pre_watchdog_domain = 0;
    power_management->pre_watchdog_cusum_max = 0.0f;
    power_management->pre_watchdog_recoveries = 0;
    power_management->last_pre_watchdog_action_us = 0;
    power_management->vrm_hiccup_count = 0;
    power_management->last_vrm_hiccup_status = 0;
    power_management->vrm_hiccup_stepdown_pending = false;
    reset_watchdog_state(power_management);
    reset_thermal_throttle_state(power_management);
    reset_vdroop_state(power_management);
    reset_dvfs_state(power_management, esp_timer_get_time());

    POWER_MANAGEMENT_init_frequency(GLOBAL_STATE);

    bool is_poweron = (esp_reset_reason() == ESP_RST_POWERON);
    if (pcm_rtc_diag_restore_or_reset(&s_rtc_diag,
                                      is_poweron,
                                      power_management->target_frequency,
                                      power_management->target_voltage,
                                      power_management->dvfs_profile)) {
        power_management->watchdog_resets = s_rtc_diag.watchdog_resets;
        power_management->cml_error_count = s_rtc_diag.cml_error_count;
        power_management->last_watchdog_reason = s_rtc_diag.last_watchdog_reason;
        power_management->watchdog_trigger_reason = PCM_WATCHDOG_REASON_NONE;
        power_management->learned_max_freq_mhz = s_rtc_diag.learned_max_freq_mhz;
        power_management->learned_min_vcore_mv = s_rtc_diag.learned_min_vcore_mv;
        power_management->adaptive_best_freq_mhz = s_rtc_diag.adaptive_best_freq_mhz;
        power_management->adaptive_best_vcore_mv = s_rtc_diag.adaptive_best_vcore_mv;
        ESP_LOGI(TAG,
                 "Restored RTC diagnostics after warm reset: watchdog_resets=%lu, last_reason=%u, cml_errors=%lu, learned_max_freq=%.1f MHz, learned_min_vcore=%u mV, adaptive_best=%.1f MHz/%u mV",
                 (unsigned long) power_management->watchdog_resets,
                 power_management->last_watchdog_reason,
                 (unsigned long) power_management->cml_error_count,
                 power_management->learned_max_freq_mhz,
                 power_management->learned_min_vcore_mv,
                 power_management->adaptive_best_freq_mhz,
                 power_management->adaptive_best_vcore_mv);
    } else {
        power_management->watchdog_resets = 0;
        power_management->cml_error_count = 0;
        power_management->last_watchdog_reason = PCM_WATCHDOG_REASON_NONE;
        power_management->watchdog_trigger_reason = PCM_WATCHDOG_REASON_NONE;
        power_management->learned_max_freq_mhz = 0.0f;
        power_management->learned_min_vcore_mv = 0;
        power_management->adaptive_best_freq_mhz = 0.0f;
        power_management->adaptive_best_vcore_mv = 0;
    }

    DvfsBanditState * bandit = ensure_bandit_state_allocated();
    if (bandit != NULL) {
        dvfs_bandit_init(bandit,
                         power_management->target_frequency,
                         power_management->target_voltage,
                         power_management->adaptive_best_freq_mhz,
                         power_management->adaptive_best_vcore_mv);
        if (power_management->dvfs_profile == DVFS_PROFILE_ADAPTIVE) {
            power_management->dvfs_freq_offset_mhz = dvfs_bandit_idx_to_freq(bandit->cur_freq_idx) - power_management->target_frequency;
            power_management->dvfs_voltage_offset_mv = (int16_t) ((int32_t) dvfs_bandit_idx_to_vcore(bandit->cur_vcore_idx) - (int32_t) power_management->target_voltage);
        }
    }

    float last_nvs_asic_frequency = power_management->target_frequency;
    uint16_t last_nvs_asic_voltage = power_management->target_voltage;
    uint16_t last_nvs_dvfs_profile = power_management->dvfs_profile;
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
        uint16_t raw_dvfs_profile = GLOBAL_STATE->SELF_TEST_MODULE.is_active
                                        ? DVFS_PROFILE_MANUAL
                                        : nvs_config_get_u16(NVS_CONFIG_DVFS_PROFILE);
        if (raw_dvfs_profile > DVFS_PROFILE_ADAPTIVE) {
            raw_dvfs_profile = DVFS_PROFILE_MANUAL;
        }
        power_management->target_voltage = raw_target_voltage;
        power_management->target_frequency = raw_target_frequency;
        power_management->dvfs_profile = raw_dvfs_profile;

        int64_t now_us = esp_timer_get_time();
        bool freq_changed = fabsf(raw_target_frequency - last_nvs_asic_frequency) > 0.001f;
        bool volt_changed = raw_target_voltage != last_nvs_asic_voltage;
        bool profile_changed = raw_dvfs_profile != last_nvs_dvfs_profile;

        if (freq_changed) {
            ESP_LOGI(TAG, "New ASIC target frequency requested: %g MHz (previous NVS: %g MHz)",
                     raw_target_frequency, last_nvs_asic_frequency);
            reset_watchdog_state(power_management);
            last_nvs_asic_frequency = raw_target_frequency;
        }
        if (volt_changed) {
            ESP_LOGI(TAG, "New ASIC target voltage requested: %u mV (previous NVS: %u mV)",
                     raw_target_voltage, last_nvs_asic_voltage);
            last_nvs_asic_voltage = raw_target_voltage;
        }
        if (profile_changed) {
            ESP_LOGI(TAG, "DVFS profile changed: %u -> %u", last_nvs_dvfs_profile, raw_dvfs_profile);
            last_nvs_dvfs_profile = raw_dvfs_profile;
        }
        if (freq_changed || volt_changed || profile_changed) {
            power_management->pre_watchdog_trigger = false;
            power_management->last_pre_watchdog_action_us = 0;
            reset_dvfs_state(power_management, now_us);
            sync_rtc_diagnostics(power_management);
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
            power_management->pre_watchdog_trigger = false;
            power_management->last_dvfs_step_us = now_us;
            vTaskDelay(POLL_RATE / portTICK_PERIOD_MS);
            continue;
        }

        handle_thermal_state_machine(GLOBAL_STATE, &prev_asic_initialized);

        if (!sys_module->overheat_mode) {
            handle_dvfs_autotuner(GLOBAL_STATE);
            handle_vdroop_compensation(GLOBAL_STATE);
            handle_pre_watchdog(GLOBAL_STATE);
            apply_effective_power_and_freq(GLOBAL_STATE);
        } else {
            power_management->pre_watchdog_trigger = false;
            reset_dvfs_state(power_management, esp_timer_get_time());
            if (s_rtc_diag.learned_max_freq_mhz != 0.0f || s_rtc_diag.learned_min_vcore_mv != 0 ||
                s_rtc_diag.adaptive_best_freq_mhz != 0.0f || s_rtc_diag.adaptive_best_vcore_mv != 0) {
                sync_rtc_diagnostics(power_management);
            }
        }

        VCORE_check_fault(GLOBAL_STATE);
        handle_vrm_hiccup_protection(GLOBAL_STATE);
        if (power_management->cml_error_count != s_rtc_diag.cml_error_count) {
            sync_rtc_diagnostics(power_management);
        }

        if (power_management->asic_watchdog_trigger) {
            power_management->asic_watchdog_trigger = false;
            power_management->pre_watchdog_trigger = false;

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
