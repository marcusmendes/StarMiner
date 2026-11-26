#ifndef POWER_CONTROL_MATH_H
#define POWER_CONTROL_MATH_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PCM_VCORE_MIN_MV 1000
#define PCM_VCORE_MAX_MV 1250
#define PCM_FREQ_MIN_MHZ 400.0f
#define PCM_MAX_POWER_W  32.0f

#define PCM_VDROOP_STEP_MV              5
#define PCM_VDROOP_MAX_OFFSET_MV        20
#define PCM_VDROOP_DEADBAND_MV          5.0f
#define PCM_VDROOP_VALID_VOLTAGE_MIN_MV 500.0f

#define PCM_DVFS_EFFICIENCY_MIN_FREQ_MHZ  500.0f
#define PCM_DVFS_EFFICIENCY_MAX_FREQ_MHZ  600.0f
#define PCM_DVFS_BALANCED_MIN_FREQ_MHZ    525.0f
#define PCM_DVFS_BALANCED_MAX_FREQ_MHZ    625.0f
#define PCM_DVFS_PERFORMANCE_MIN_FREQ_MHZ 550.0f
#define PCM_DVFS_PERFORMANCE_MAX_FREQ_MHZ 675.0f
#define PCM_DVFS_ADAPTIVE_MIN_FREQ_MHZ    500.0f
#define PCM_DVFS_ADAPTIVE_MAX_FREQ_MHZ    675.0f

#define PCM_DVFS_FREQ_STEP_MHZ            12.5f
#define PCM_DVFS_VCORE_STEP_DOWN_MV       5
#define PCM_DVFS_VCORE_STEP_UP_MV         10

#define PCM_DVFS_EFFICIENCY_MAX_ERROR_PCT 0.5f

#define PCM_DVFS_BALANCED_MAX_ERROR_PCT      0.8f
#define PCM_DVFS_BALANCED_MAX_ASIC_TEMP      64.0f
#define PCM_DVFS_BALANCED_MAX_VR_TEMP        80.0f
#define PCM_DVFS_BALANCED_PULLBACK_ASIC_TEMP 65.5f
#define PCM_DVFS_BALANCED_PULLBACK_VR_TEMP   84.0f

#define PCM_DVFS_PERFORMANCE_MAX_ERROR_PCT      1.0f
#define PCM_DVFS_PERFORMANCE_MAX_ASIC_TEMP      68.0f
#define PCM_DVFS_PERFORMANCE_MAX_VR_TEMP        90.0f
#define PCM_DVFS_PERFORMANCE_MAX_POWER_W        31.0f
#define PCM_DVFS_PERFORMANCE_PULLBACK_ASIC_TEMP 66.5f
#define PCM_DVFS_PERFORMANCE_PULLBACK_VR_TEMP   87.0f
#define PCM_DVFS_PERFORMANCE_PULLBACK_POWER_W   31.2f

#define PCM_EMC2101_FAN_RPM_NUMERATOR 5400000U
#define PCM_VR_TO_ASIC_TEMP_OFFSET_C  22.0f

#define PCM_CUSUM_MAX_DOMAINS           4
#define PCM_CUSUM_ALPHA                 0.20f
#define PCM_CUSUM_RATIO_MAX             1.50f
#define PCM_CUSUM_DEFAULT_DRIFT_K       0.78f
#define PCM_CUSUM_DEFAULT_THRESHOLD_H   2.0f

#define PCM_PRE_WATCHDOG_VCORE_STEP_MV  5
#define PCM_PRE_WATCHDOG_MAX_POWER_W    30.5f
#define PCM_PRE_WATCHDOG_MAX_VR_TEMP_C  84.0f
#define PCM_PRE_WATCHDOG_COOLDOWN_US    (60LL * 1000000LL)

#define PCM_RTC_DIAG_MAGIC 0x53545245U /* "STRE" */

typedef enum {
    PCM_WATCHDOG_REASON_NONE = 0,
    PCM_WATCHDOG_REASON_ZERO_HASHRATE = 1,
    PCM_WATCHDOG_REASON_FROZEN_DOMAIN = 2,
    PCM_WATCHDOG_REASON_LEVEL3_REBOOT = 3,
} PcmWatchdogReason;

typedef struct {
    uint32_t magic;
    uint32_t crc32;
    uint16_t target_voltage_mv;
    uint16_t dvfs_profile;
    float target_frequency_mhz;
    uint32_t watchdog_resets;
    uint32_t cml_error_count;
    uint16_t learned_min_vcore_mv;
    uint8_t last_watchdog_reason;
    uint8_t reserved;
    float learned_max_freq_mhz;
    float adaptive_best_freq_mhz;
    uint16_t adaptive_best_vcore_mv;
    uint16_t reserved2;
} PcmRtcDiagnostics;

typedef enum {
    PCM_DVFS_PROFILE_MANUAL = 0,
    PCM_DVFS_PROFILE_EFFICIENCY = 1,
    PCM_DVFS_PROFILE_BALANCED = 2,
    PCM_DVFS_PROFILE_PERFORMANCE = 3,
    PCM_DVFS_PROFILE_ADAPTIVE = 4,
} PcmDvfsProfile;

typedef struct {
    float ewma_ratio[PCM_CUSUM_MAX_DOMAINS];
    float cusum[PCM_CUSUM_MAX_DOMAINS];
} PcmDomainCusumState;

typedef enum {
    PCM_PRE_WATCHDOG_ACTION_NONE = 0,
    PCM_PRE_WATCHDOG_ACTION_VCORE_STEP_UP = 1,
    PCM_PRE_WATCHDOG_ACTION_FREQ_STEP_DOWN = 2,
} PcmPreWatchdogAction;

/**
 * @brief Clamps an ASIC core voltage target to hardware safety limits.
 */
uint16_t pcm_clamp_vcore_mv(int32_t voltage_mv);

/**
 * @brief Outputs frequency envelope bounds for DVFS profiles 1..3.
 *
 * @return true if profile has enforced DVFS bounds, false for Manual/unknown.
 */
bool pcm_get_dvfs_freq_bounds(uint16_t profile, float * min_freq, float * max_freq);

/**
 * @brief Computes base target frequency after applying and clamping DVFS offset.
 */
float pcm_compute_dvfs_base_freq(uint16_t profile, float target_frequency_mhz, float dvfs_freq_offset_mhz);

/**
 * @brief Computes effective ASIC frequency with thermal/watchdog offsets and 400 MHz floor.
 */
float pcm_compute_effective_freq(uint16_t profile,
                                 float target_frequency_mhz,
                                 float dvfs_freq_offset_mhz,
                                 float thermal_offset_mhz,
                                 float watchdog_freq_offset_mhz);

/**
 * @brief Computes effective VCore with DVFS, thermal, and Vdroop offsets clamped to safety limits.
 */
uint16_t pcm_compute_effective_vcore(uint16_t target_voltage_mv,
                                     int16_t dvfs_voltage_offset_mv,
                                     uint16_t thermal_offset_mv,
                                     uint16_t vdroop_offset_mv);

/**
 * @brief Computes effective VCore floor considering any per-chip learned minimum.
 */
uint16_t pcm_dvfs_effective_vcore_floor(uint16_t learned_min_vcore_mv);

/**
 * @brief Computes effective DVFS frequency ceiling within profile bounds considering
 *        any per-chip learned maximum frequency (<= 0 or NaN means unlocked).
 */
float pcm_dvfs_effective_freq_ceiling(float profile_min_freq_mhz,
                                      float profile_max_freq_mhz,
                                      float learned_max_freq_mhz);

/**
 * @brief Executes one 60 s evaluation step for DVFS Efficiency profile (profile 1)
 *        with per-chip learned minimum VCore floor locking.
 *
 * @return Resulting base target VCore in mV.
 */
uint16_t pcm_dvfs_step_efficiency(uint16_t target_voltage_mv,
                                  int16_t * dvfs_voltage_offset_mv,
                                  uint16_t * learned_min_vcore_mv,
                                  float error_percentage,
                                  float power_w);

/**
 * @brief Executes one 60 s evaluation step for DVFS Balanced profile (profile 2),
 *        enforcing fixed NVS voltage, per-chip learned max frequency ceiling, and
 *        preventive thermal pullback before soft-throttling.
 *
 * @return Resulting base target frequency in MHz.
 */
float pcm_dvfs_step_balanced(float target_frequency_mhz,
                             float * dvfs_freq_offset_mhz,
                             int16_t * dvfs_voltage_offset_mv,
                             float * learned_max_freq_mhz,
                             float error_percentage,
                             float chip_temp_c,
                             float vr_temp_c,
                             float power_w);

/**
 * @brief Executes one 60 s evaluation step for DVFS Performance profile (profile 3)
 *        with 2D silicon learning (VCore floor + frequency ceiling) and preventive
 *        thermal/power pullback.
 *
 * @return Resulting base target frequency in MHz.
 */
float pcm_dvfs_step_performance(float target_frequency_mhz,
                                uint16_t target_voltage_mv,
                                float * dvfs_freq_offset_mhz,
                                int16_t * dvfs_voltage_offset_mv,
                                float * learned_max_freq_mhz,
                                uint16_t * learned_min_vcore_mv,
                                float error_percentage,
                                float chip_temp_c,
                                float vr_temp_c,
                                float power_w);

/**
 * @brief Updates DVFS learned bounds and RAM offsets when the ASIC Watchdog triggers
 *        Level 1 or Level 2 recovery, preventing DVFS from climbing back into an
 *        unstable operating point.
 */
void pcm_dvfs_apply_watchdog_event(uint16_t profile,
                                   uint8_t consecutive_failures,
                                   float target_frequency_mhz,
                                   uint16_t target_voltage_mv,
                                   float * dvfs_freq_offset_mhz,
                                   int16_t * dvfs_voltage_offset_mv,
                                   float * watchdog_freq_offset_mhz,
                                   float * learned_max_freq_mhz,
                                   uint16_t * learned_min_vcore_mv);

/**
 * @brief Computes the next slow-loop Vdroop compensation offset in mV.
 */
uint16_t pcm_step_vdroop_offset_mv(uint16_t current_offset_mv,
                                   uint16_t base_voltage_mv,
                                   float measured_voltage_mv,
                                   float power_w);

/**
 * @brief Converts a raw 16-bit EMC2101 tachometer reading to fan RPM.
 *
 * Returns 0 for invalid/stalled readings (0x0000, 0xFFFF, or saturated RPM == 82).
 */
uint16_t pcm_emc2101_tach_to_rpm(uint16_t reading);

/**
 * @brief Computes composite virtual temperature for the fan PID controller:
 *        max(valid ASIC temps, vr_temp - 22.0 C) when vr_temp > 0 C.
 *
 * Returns 0.0f when no valid positive temperature is available.
 */
float pcm_compute_pid_effective_temp(float chip1_temp, float chip2_temp, float vr_temp);

/**
 * @brief Populates magic, NVS target fingerprint, and CRC32 for an RTC diagnostics block.
 */
void pcm_rtc_diag_seal(PcmRtcDiagnostics * diag,
                       float target_frequency_mhz,
                       uint16_t target_voltage_mv,
                       uint16_t dvfs_profile);

/**
 * @brief Validates magic, NVS target fingerprint, value bounds, and CRC32 of an RTC diagnostics block.
 */
bool pcm_rtc_diag_validate(const PcmRtcDiagnostics * diag,
                           float target_frequency_mhz,
                           uint16_t target_voltage_mv,
                           uint16_t dvfs_profile);

/**
 * @brief Validates and preserves RTC diagnostics on warm reset (!is_poweron_reset), or zeroes
 *        and seals a clean record on cold boot (ESP_RST_POWERON), CRC corruption, or NVS target change.
 *
 * @return true if existing state was valid and preserved, false if reset to zero.
 */
bool pcm_rtc_diag_restore_or_reset(PcmRtcDiagnostics * diag,
                                   bool is_poweron_reset,
                                   float target_frequency_mhz,
                                   uint16_t target_voltage_mv,
                                   uint16_t dvfs_profile);

/**
 * @brief Resets all domain EWMA ratios to 1.0f and unilateral CUSUM accumulators to 0.0f.
 */
void pcm_domain_cusum_reset(PcmDomainCusumState * state);

/**
 * @brief Updates 1 Hz per-domain EWMA ratio against peer average and unilateral CUSUM accumulator.
 *
 * @return true when any domain crosses cusum >= threshold_h (populating out_domain and out_cusum).
 */
bool pcm_domain_cusum_update(PcmDomainCusumState * state,
                             const float * domain_hr,
                             int domain_count,
                             float drift_k,
                             float threshold_h,
                             uint8_t * out_domain,
                             float * out_cusum);

/**
 * @brief Applies profile-aware hot relief in RAM (+5 mV VCore or -12.5 MHz frequency)
 *        when the domain CUSUM pre-watchdog triggers, without stopping the ASIC.
 */
PcmPreWatchdogAction pcm_pre_watchdog_apply_relief(uint16_t profile,
                                                   float target_frequency_mhz,
                                                   uint16_t target_voltage_mv,
                                                   uint16_t vdroop_offset_mv,
                                                   float power_w,
                                                   float vr_temp_c,
                                                   float * dvfs_freq_offset_mhz,
                                                   int16_t * dvfs_voltage_offset_mv,
                                                   float * watchdog_freq_offset_mhz,
                                                   float * learned_max_freq_mhz,
                                                   uint16_t * learned_min_vcore_mv);

#ifdef __cplusplus
}
#endif

#endif /* POWER_CONTROL_MATH_H */
