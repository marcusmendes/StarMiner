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

#define PCM_EMC2101_FAN_RPM_NUMERATOR 5400000U

typedef enum {
    PCM_DVFS_PROFILE_MANUAL = 0,
    PCM_DVFS_PROFILE_EFFICIENCY = 1,
    PCM_DVFS_PROFILE_BALANCED = 2,
    PCM_DVFS_PROFILE_PERFORMANCE = 3,
} PcmDvfsProfile;

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

#ifdef __cplusplus
}
#endif

#endif /* POWER_CONTROL_MATH_H */
