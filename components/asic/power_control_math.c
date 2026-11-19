#include "power_control_math.h"

#include <math.h>
#include <stddef.h>

uint16_t pcm_clamp_vcore_mv(int32_t voltage_mv)
{
    if (voltage_mv < PCM_VCORE_MIN_MV) {
        return PCM_VCORE_MIN_MV;
    }
    if (voltage_mv > PCM_VCORE_MAX_MV) {
        return PCM_VCORE_MAX_MV;
    }
    return (uint16_t) voltage_mv;
}

bool pcm_get_dvfs_freq_bounds(uint16_t profile, float * min_freq, float * max_freq)
{
    if (min_freq == NULL || max_freq == NULL) {
        return false;
    }

    switch (profile) {
        case PCM_DVFS_PROFILE_EFFICIENCY:
            *min_freq = PCM_DVFS_EFFICIENCY_MIN_FREQ_MHZ;
            *max_freq = PCM_DVFS_EFFICIENCY_MAX_FREQ_MHZ;
            return true;
        case PCM_DVFS_PROFILE_BALANCED:
            *min_freq = PCM_DVFS_BALANCED_MIN_FREQ_MHZ;
            *max_freq = PCM_DVFS_BALANCED_MAX_FREQ_MHZ;
            return true;
        case PCM_DVFS_PROFILE_PERFORMANCE:
            *min_freq = PCM_DVFS_PERFORMANCE_MIN_FREQ_MHZ;
            *max_freq = PCM_DVFS_PERFORMANCE_MAX_FREQ_MHZ;
            return true;
        default:
            return false;
    }
}

float pcm_compute_dvfs_base_freq(uint16_t profile, float target_frequency_mhz, float dvfs_freq_offset_mhz)
{
    float min_freq = PCM_FREQ_MIN_MHZ;
    float max_freq = PCM_DVFS_PERFORMANCE_MAX_FREQ_MHZ;

    if (!pcm_get_dvfs_freq_bounds(profile, &min_freq, &max_freq)) {
        return target_frequency_mhz;
    }

    float raw_freq = target_frequency_mhz + dvfs_freq_offset_mhz;
    if (raw_freq < min_freq) {
        return min_freq;
    }
    if (raw_freq > max_freq) {
        return max_freq;
    }
    return raw_freq;
}

float pcm_compute_effective_freq(uint16_t profile,
                                 float target_frequency_mhz,
                                 float dvfs_freq_offset_mhz,
                                 float thermal_offset_mhz,
                                 float watchdog_freq_offset_mhz)
{
    float base_target_freq = pcm_compute_dvfs_base_freq(profile, target_frequency_mhz, dvfs_freq_offset_mhz);
    float total_offset = thermal_offset_mhz + watchdog_freq_offset_mhz;
    return fmaxf(PCM_FREQ_MIN_MHZ, base_target_freq - total_offset);
}

uint16_t pcm_compute_effective_vcore(uint16_t target_voltage_mv,
                                     int16_t dvfs_voltage_offset_mv,
                                     uint16_t thermal_offset_mv,
                                     uint16_t vdroop_offset_mv)
{
    uint16_t dvfs_target_mv = pcm_clamp_vcore_mv((int32_t) target_voltage_mv + (int32_t) dvfs_voltage_offset_mv);
    uint16_t base_voltage_mv = pcm_clamp_vcore_mv((int32_t) dvfs_target_mv - (int32_t) thermal_offset_mv);
    return pcm_clamp_vcore_mv((int32_t) base_voltage_mv + (int32_t) vdroop_offset_mv);
}

uint16_t pcm_step_vdroop_offset_mv(uint16_t current_offset_mv,
                                   uint16_t base_voltage_mv,
                                   float measured_voltage_mv,
                                   float power_w)
{
    if (isnan(measured_voltage_mv) || measured_voltage_mv <= PCM_VDROOP_VALID_VOLTAGE_MIN_MV) {
        return current_offset_mv;
    }

    uint16_t clamped_base_mv = pcm_clamp_vcore_mv((int32_t) base_voltage_mv);
    uint16_t max_headroom = (PCM_VCORE_MAX_MV > clamped_base_mv) ? (PCM_VCORE_MAX_MV - clamped_base_mv) : 0;
    uint16_t max_vdroop = (max_headroom < PCM_VDROOP_MAX_OFFSET_MV) ? max_headroom : PCM_VDROOP_MAX_OFFSET_MV;
    uint16_t offset_mv = (current_offset_mv > max_vdroop) ? max_vdroop : current_offset_mv;

    if (measured_voltage_mv < ((float) clamped_base_mv - PCM_VDROOP_DEADBAND_MV)) {
        if (!isnan(power_w) && power_w <= PCM_MAX_POWER_W && offset_mv < max_vdroop) {
            uint16_t next_offset = (uint16_t) (offset_mv + PCM_VDROOP_STEP_MV);
            offset_mv = (next_offset > max_vdroop) ? max_vdroop : next_offset;
        }
    } else if (measured_voltage_mv > ((float) clamped_base_mv + PCM_VDROOP_DEADBAND_MV) && offset_mv > 0) {
        offset_mv = (offset_mv >= PCM_VDROOP_STEP_MV)
                        ? (uint16_t) (offset_mv - PCM_VDROOP_STEP_MV)
                        : 0;
    }

    return offset_mv;
}

uint16_t pcm_emc2101_tach_to_rpm(uint16_t reading)
{
    if (reading == 0 || reading == 0xFFFF) {
        return 0;
    }

    uint32_t rpm = PCM_EMC2101_FAN_RPM_NUMERATOR / (uint32_t) reading;
    if (rpm == 82U) {
        return 0;
    }
    if (rpm > UINT16_MAX) {
        return UINT16_MAX;
    }
    return (uint16_t) rpm;
}

float pcm_compute_pid_effective_temp(float chip1_temp, float chip2_temp, float vr_temp)
{
    float effective_temp = 0.0f;

    if (!isnan(chip1_temp) && chip1_temp > 0.0f) {
        effective_temp = chip1_temp;
    }
    if (!isnan(chip2_temp) && chip2_temp > 0.0f) {
        effective_temp = fmaxf(effective_temp, chip2_temp);
    }
    if (!isnan(vr_temp) && vr_temp > 0.0f) {
        float vr_equiv = vr_temp - PCM_VR_TO_ASIC_TEMP_OFFSET_C;
        if (vr_equiv > effective_temp) {
            effective_temp = vr_equiv;
        }
    }

    return effective_temp;
}
