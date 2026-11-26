#include "power_control_math.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

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
        case PCM_DVFS_PROFILE_ADAPTIVE:
            *min_freq = PCM_DVFS_ADAPTIVE_MIN_FREQ_MHZ;
            *max_freq = PCM_DVFS_ADAPTIVE_MAX_FREQ_MHZ;
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

static void lock_learned_max_freq(float * learned_max_freq_mhz, float candidate_mhz)
{
    if (learned_max_freq_mhz == NULL) {
        return;
    }
    if (isnan(*learned_max_freq_mhz) || *learned_max_freq_mhz <= 0.0f || candidate_mhz < *learned_max_freq_mhz) {
        *learned_max_freq_mhz = candidate_mhz;
    }
}

static void lock_learned_min_vcore(uint16_t * learned_min_vcore_mv, uint16_t candidate_mv)
{
    if (learned_min_vcore_mv == NULL) {
        return;
    }
    if (candidate_mv > *learned_min_vcore_mv) {
        *learned_min_vcore_mv = candidate_mv;
    }
}

static void step_watchdog_freq_offset(float cur_base_freq, float * wd_freq_off)
{
    if (wd_freq_off == NULL) {
        return;
    }
    if (cur_base_freq > PCM_FREQ_MIN_MHZ) {
        float max_offset = cur_base_freq - PCM_FREQ_MIN_MHZ;
        *wd_freq_off = fminf(max_offset, *wd_freq_off + PCM_DVFS_FREQ_STEP_MHZ);
    }
}

uint16_t pcm_dvfs_effective_vcore_floor(uint16_t learned_min_vcore_mv)
{
    if (learned_min_vcore_mv > PCM_VCORE_MIN_MV) {
        return pcm_clamp_vcore_mv((int32_t) learned_min_vcore_mv);
    }
    return PCM_VCORE_MIN_MV;
}

float pcm_dvfs_effective_freq_ceiling(float profile_min_freq_mhz,
                                      float profile_max_freq_mhz,
                                      float learned_max_freq_mhz)
{
    if (isnan(learned_max_freq_mhz) || learned_max_freq_mhz <= 0.0f) {
        return profile_max_freq_mhz;
    }
    return fmaxf(profile_min_freq_mhz, fminf(profile_max_freq_mhz, learned_max_freq_mhz));
}

uint16_t pcm_dvfs_step_efficiency(uint16_t target_voltage_mv,
                                  int16_t * dvfs_voltage_offset_mv,
                                  uint16_t * learned_min_vcore_mv,
                                  float error_percentage,
                                  float power_w)
{
    int16_t volt_offset = (dvfs_voltage_offset_mv != NULL) ? *dvfs_voltage_offset_mv : 0;
    uint16_t learned_floor = (learned_min_vcore_mv != NULL) ? *learned_min_vcore_mv : 0;
    uint16_t cur_mv = pcm_compute_effective_vcore(target_voltage_mv, volt_offset, 0, 0);
    uint16_t next_mv = cur_mv;
    float err_pct = isnan(error_percentage) ? 0.0f : error_percentage;

    if (err_pct <= PCM_DVFS_EFFICIENCY_MAX_ERROR_PCT) {
        uint16_t effective_floor = pcm_dvfs_effective_vcore_floor(learned_floor);
        if (cur_mv > effective_floor) {
            uint16_t stepped = (cur_mv >= PCM_DVFS_VCORE_STEP_DOWN_MV)
                                   ? (uint16_t) (cur_mv - PCM_DVFS_VCORE_STEP_DOWN_MV)
                                   : 0;
            next_mv = (stepped < effective_floor) ? effective_floor : stepped;
        }
    } else if (!isnan(power_w) && power_w <= PCM_MAX_POWER_W) {
        next_mv = pcm_clamp_vcore_mv((int32_t) cur_mv + PCM_DVFS_VCORE_STEP_UP_MV);
        lock_learned_min_vcore(learned_min_vcore_mv, next_mv);
    }

    if (dvfs_voltage_offset_mv != NULL && next_mv != cur_mv) {
        *dvfs_voltage_offset_mv = (int16_t) ((int32_t) next_mv - (int32_t) target_voltage_mv);
    }
    return next_mv;
}

float pcm_dvfs_step_balanced(float target_frequency_mhz,
                             float * dvfs_freq_offset_mhz,
                             int16_t * dvfs_voltage_offset_mv,
                             float * learned_max_freq_mhz,
                             float error_percentage,
                             float chip_temp_c,
                             float vr_temp_c,
                             float power_w)
{
    if (dvfs_voltage_offset_mv != NULL) {
        *dvfs_voltage_offset_mv = 0;
    }

    float freq_offset = (dvfs_freq_offset_mhz != NULL) ? *dvfs_freq_offset_mhz : 0.0f;
    float learned_ceiling = (learned_max_freq_mhz != NULL) ? *learned_max_freq_mhz : 0.0f;
    float cur_freq = pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_BALANCED, target_frequency_mhz, freq_offset);
    float clamped_target_freq = pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_BALANCED, target_frequency_mhz, 0.0f);
    float next_freq = cur_freq;

    float err_pct = isnan(error_percentage) ? 0.0f : error_percentage;
    bool chip_valid = !isnan(chip_temp_c) && chip_temp_c > 0.0f;
    bool vr_valid = !isnan(vr_temp_c) && vr_temp_c >= 0.0f;
    bool power_valid = !isnan(power_w);

    if (err_pct > PCM_DVFS_BALANCED_MAX_ERROR_PCT) {
        next_freq = fmaxf(PCM_DVFS_BALANCED_MIN_FREQ_MHZ, cur_freq - PCM_DVFS_FREQ_STEP_MHZ);
        lock_learned_max_freq(learned_max_freq_mhz, next_freq);
    } else if ((chip_valid && chip_temp_c >= PCM_DVFS_BALANCED_PULLBACK_ASIC_TEMP) ||
               (vr_valid && vr_temp_c >= PCM_DVFS_BALANCED_PULLBACK_VR_TEMP)) {
        if (cur_freq > (clamped_target_freq + 0.001f)) {
            next_freq = fmaxf(clamped_target_freq, cur_freq - PCM_DVFS_FREQ_STEP_MHZ);
        }
    } else if ((chip_valid && chip_temp_c < PCM_DVFS_BALANCED_MAX_ASIC_TEMP) &&
               (vr_valid && vr_temp_c < PCM_DVFS_BALANCED_MAX_VR_TEMP) &&
               (power_valid && power_w <= PCM_MAX_POWER_W)) {
        float ceiling = pcm_dvfs_effective_freq_ceiling(PCM_DVFS_BALANCED_MIN_FREQ_MHZ,
                                                        PCM_DVFS_BALANCED_MAX_FREQ_MHZ,
                                                        learned_ceiling);
        if (cur_freq < ceiling) {
            next_freq = fminf(ceiling, cur_freq + PCM_DVFS_FREQ_STEP_MHZ);
        }
    }

    if (dvfs_freq_offset_mhz != NULL && fabsf(next_freq - cur_freq) > 0.001f) {
        *dvfs_freq_offset_mhz = next_freq - target_frequency_mhz;
    }
    return next_freq;
}

float pcm_dvfs_step_performance(float target_frequency_mhz,
                                uint16_t target_voltage_mv,
                                float * dvfs_freq_offset_mhz,
                                int16_t * dvfs_voltage_offset_mv,
                                float * learned_max_freq_mhz,
                                uint16_t * learned_min_vcore_mv,
                                float error_percentage,
                                float chip_temp_c,
                                float vr_temp_c,
                                float power_w)
{
    float freq_offset = (dvfs_freq_offset_mhz != NULL) ? *dvfs_freq_offset_mhz : 0.0f;
    int16_t volt_offset = (dvfs_voltage_offset_mv != NULL) ? *dvfs_voltage_offset_mv : 0;
    float learned_ceiling = (learned_max_freq_mhz != NULL) ? *learned_max_freq_mhz : 0.0f;

    float cur_freq = pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_PERFORMANCE, target_frequency_mhz, freq_offset);
    float clamped_target_freq = pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_PERFORMANCE, target_frequency_mhz, 0.0f);
    uint16_t cur_mv = pcm_compute_effective_vcore(target_voltage_mv, volt_offset, 0, 0);

    float next_freq = cur_freq;
    uint16_t next_mv = cur_mv;

    float err_pct = isnan(error_percentage) ? 0.0f : error_percentage;
    bool chip_valid = !isnan(chip_temp_c) && chip_temp_c > 0.0f;
    bool vr_valid = !isnan(vr_temp_c) && vr_temp_c >= 0.0f;
    bool power_valid = !isnan(power_w);
    bool power_ok = power_valid && power_w < PCM_DVFS_PERFORMANCE_MAX_POWER_W;

    if (err_pct > PCM_DVFS_PERFORMANCE_MAX_ERROR_PCT) {
        if (cur_mv < PCM_VCORE_MAX_MV && power_ok) {
            next_mv = pcm_clamp_vcore_mv((int32_t) cur_mv + PCM_DVFS_VCORE_STEP_UP_MV);
            lock_learned_min_vcore(learned_min_vcore_mv, next_mv);
        } else {
            next_freq = fmaxf(PCM_DVFS_PERFORMANCE_MIN_FREQ_MHZ, cur_freq - PCM_DVFS_FREQ_STEP_MHZ);
            lock_learned_max_freq(learned_max_freq_mhz, next_freq);
        }
    } else if ((chip_valid && chip_temp_c >= PCM_DVFS_PERFORMANCE_PULLBACK_ASIC_TEMP) ||
               (vr_valid && vr_temp_c >= PCM_DVFS_PERFORMANCE_PULLBACK_VR_TEMP) ||
               (power_valid && power_w >= PCM_DVFS_PERFORMANCE_PULLBACK_POWER_W)) {
        if (cur_freq > (clamped_target_freq + 0.001f)) {
            next_freq = fmaxf(clamped_target_freq, cur_freq - PCM_DVFS_FREQ_STEP_MHZ);
        }
    } else if ((chip_valid && chip_temp_c < PCM_DVFS_PERFORMANCE_MAX_ASIC_TEMP) &&
               (vr_valid && vr_temp_c < PCM_DVFS_PERFORMANCE_MAX_VR_TEMP) &&
               power_ok) {
        float ceiling = pcm_dvfs_effective_freq_ceiling(PCM_DVFS_PERFORMANCE_MIN_FREQ_MHZ,
                                                        PCM_DVFS_PERFORMANCE_MAX_FREQ_MHZ,
                                                        learned_ceiling);
        if (cur_freq < ceiling) {
            next_freq = fminf(ceiling, cur_freq + PCM_DVFS_FREQ_STEP_MHZ);
        }
    }

    if (dvfs_voltage_offset_mv != NULL && next_mv != cur_mv) {
        *dvfs_voltage_offset_mv = (int16_t) ((int32_t) next_mv - (int32_t) target_voltage_mv);
    }
    if (dvfs_freq_offset_mhz != NULL && fabsf(next_freq - cur_freq) > 0.001f) {
        *dvfs_freq_offset_mhz = next_freq - target_frequency_mhz;
    }
    return next_freq;
}

void pcm_dvfs_apply_watchdog_event(uint16_t profile,
                                   uint8_t consecutive_failures,
                                   float target_frequency_mhz,
                                   uint16_t target_voltage_mv,
                                   float * dvfs_freq_offset_mhz,
                                   int16_t * dvfs_voltage_offset_mv,
                                   float * watchdog_freq_offset_mhz,
                                   float * learned_max_freq_mhz,
                                   uint16_t * learned_min_vcore_mv)
{
    if (consecutive_failures == 0) {
        return;
    }

    float dvfs_freq_off = (dvfs_freq_offset_mhz != NULL) ? *dvfs_freq_offset_mhz : 0.0f;
    int16_t dvfs_volt_off = (dvfs_voltage_offset_mv != NULL) ? *dvfs_voltage_offset_mv : 0;
    float wd_freq_off = (watchdog_freq_offset_mhz != NULL) ? *watchdog_freq_offset_mhz : 0.0f;
    float cur_base_freq = pcm_compute_dvfs_base_freq(profile, target_frequency_mhz, dvfs_freq_off);

    if (consecutive_failures == 1) {
        if (profile == PCM_DVFS_PROFILE_BALANCED || profile == PCM_DVFS_PROFILE_PERFORMANCE) {
            lock_learned_max_freq(learned_max_freq_mhz, cur_base_freq);
        }
        return;
    }

    if (profile == PCM_DVFS_PROFILE_BALANCED || profile == PCM_DVFS_PROFILE_PERFORMANCE) {
        float min_freq = PCM_FREQ_MIN_MHZ;
        float max_freq = PCM_DVFS_PERFORMANCE_MAX_FREQ_MHZ;
        pcm_get_dvfs_freq_bounds(profile, &min_freq, &max_freq);

        if (cur_base_freq > (min_freq + 0.001f)) {
            float next_freq = fmaxf(min_freq, cur_base_freq - PCM_DVFS_FREQ_STEP_MHZ);
            dvfs_freq_off = next_freq - target_frequency_mhz;
            lock_learned_max_freq(learned_max_freq_mhz, next_freq);
        } else {
            lock_learned_max_freq(learned_max_freq_mhz, min_freq);
            step_watchdog_freq_offset(cur_base_freq, &wd_freq_off);
        }
    } else if (profile == PCM_DVFS_PROFILE_EFFICIENCY) {
        step_watchdog_freq_offset(cur_base_freq, &wd_freq_off);
        uint16_t cur_mv = pcm_compute_effective_vcore(target_voltage_mv, dvfs_volt_off, 0, 0);
        uint16_t cur_learned_mv = (learned_min_vcore_mv != NULL) ? *learned_min_vcore_mv : 0;
        uint16_t base_mv = (cur_learned_mv > cur_mv) ? cur_learned_mv : cur_mv;
        uint16_t next_mv = pcm_clamp_vcore_mv((int32_t) base_mv + PCM_DVFS_VCORE_STEP_UP_MV);
        lock_learned_min_vcore(learned_min_vcore_mv, next_mv);
        dvfs_volt_off = (int16_t) ((int32_t) next_mv - (int32_t) target_voltage_mv);
    } else {
        step_watchdog_freq_offset(cur_base_freq, &wd_freq_off);
    }

    if (dvfs_freq_offset_mhz != NULL) {
        *dvfs_freq_offset_mhz = dvfs_freq_off;
    }
    if (dvfs_voltage_offset_mv != NULL) {
        *dvfs_voltage_offset_mv = dvfs_volt_off;
    }
    if (watchdog_freq_offset_mhz != NULL) {
        *watchdog_freq_offset_mhz = wd_freq_off;
    }
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

static uint32_t pcm_crc32_bytes(const uint8_t * data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFU;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t) data[i];
        for (int bit = 0; bit < 8; bit++) {
            uint32_t mask = (uint32_t) -(int32_t) (crc & 1U);
            crc = (crc >> 1) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

static uint32_t pcm_rtc_diag_compute_crc32(const PcmRtcDiagnostics * diag)
{
    PcmRtcDiagnostics copy;
    memcpy(&copy, diag, sizeof(copy));
    copy.crc32 = 0;
    copy.reserved = 0;
    copy.reserved2 = 0;
    return pcm_crc32_bytes((const uint8_t *) &copy, sizeof(copy));
}

void pcm_rtc_diag_seal(PcmRtcDiagnostics * diag,
                       float target_frequency_mhz,
                       uint16_t target_voltage_mv,
                       uint16_t dvfs_profile)
{
    if (diag == NULL) {
        return;
    }

    diag->magic = PCM_RTC_DIAG_MAGIC;
    diag->target_frequency_mhz = target_frequency_mhz;
    diag->target_voltage_mv = target_voltage_mv;
    diag->dvfs_profile = dvfs_profile;
    diag->reserved = 0;
    diag->reserved2 = 0;
    diag->crc32 = 0;
    diag->crc32 = pcm_rtc_diag_compute_crc32(diag);
}

bool pcm_rtc_diag_validate(const PcmRtcDiagnostics * diag,
                           float target_frequency_mhz,
                           uint16_t target_voltage_mv,
                           uint16_t dvfs_profile)
{
    if (diag == NULL || diag->magic != PCM_RTC_DIAG_MAGIC || diag->reserved != 0 || diag->reserved2 != 0) {
        return false;
    }
    if (isnan(diag->target_frequency_mhz) || isnan(target_frequency_mhz) ||
        fabsf(diag->target_frequency_mhz - target_frequency_mhz) > 0.001f) {
        return false;
    }
    if (diag->target_voltage_mv != target_voltage_mv || diag->dvfs_profile != dvfs_profile) {
        return false;
    }
    if (diag->last_watchdog_reason > PCM_WATCHDOG_REASON_LEVEL3_REBOOT) {
        return false;
    }
    if (isnan(diag->learned_max_freq_mhz) || diag->learned_max_freq_mhz < 0.0f) {
        return false;
    }
    if (isnan(diag->adaptive_best_freq_mhz) || diag->adaptive_best_freq_mhz < 0.0f ||
        (diag->adaptive_best_freq_mhz > 0.0f &&
         (diag->adaptive_best_freq_mhz < PCM_FREQ_MIN_MHZ ||
          diag->adaptive_best_freq_mhz > PCM_DVFS_ADAPTIVE_MAX_FREQ_MHZ))) {
        return false;
    }
    if (diag->adaptive_best_vcore_mv != 0 &&
        (diag->adaptive_best_vcore_mv < PCM_VCORE_MIN_MV ||
         diag->adaptive_best_vcore_mv > PCM_VCORE_MAX_MV)) {
        return false;
    }

    return pcm_rtc_diag_compute_crc32(diag) == diag->crc32;
}

bool pcm_rtc_diag_restore_or_reset(PcmRtcDiagnostics * diag,
                                   bool is_poweron_reset,
                                   float target_frequency_mhz,
                                   uint16_t target_voltage_mv,
                                   uint16_t dvfs_profile)
{
    if (diag == NULL) {
        return false;
    }

    if (!is_poweron_reset &&
        pcm_rtc_diag_validate(diag, target_frequency_mhz, target_voltage_mv, dvfs_profile)) {
        return true;
    }

    memset(diag, 0, sizeof(*diag));
    pcm_rtc_diag_seal(diag, target_frequency_mhz, target_voltage_mv, dvfs_profile);
    return false;
}

void pcm_domain_cusum_reset(PcmDomainCusumState * state)
{
    if (state == NULL) {
        return;
    }
    for (int d = 0; d < PCM_CUSUM_MAX_DOMAINS; d++) {
        state->ewma_ratio[d] = 1.0f;
        state->cusum[d] = 0.0f;
    }
}

bool pcm_domain_cusum_update(PcmDomainCusumState * state,
                             const float * domain_hr,
                             int domain_count,
                             float drift_k,
                             float threshold_h,
                             uint8_t * out_domain,
                             float * out_cusum)
{
    if (state == NULL || domain_hr == NULL || domain_count <= 1) {
        return false;
    }

    int count = (domain_count > PCM_CUSUM_MAX_DOMAINS) ? PCM_CUSUM_MAX_DOMAINS : domain_count;
    if (count <= 1) {
        return false;
    }

    float sum = 0.0f;
    for (int d = 0; d < count; d++) {
        if (isnan(domain_hr[d])) {
            return false;
        }
        float val = (domain_hr[d] > 0.0f) ? domain_hr[d] : 0.0f;
        sum += val;
    }

    if (sum <= 0.0001f) {
        return false;
    }

    uint8_t best_domain = 0;
    float max_s = -1.0f;

    for (int d = 0; d < count; d++) {
        float d_hr = (domain_hr[d] > 0.0f) ? domain_hr[d] : 0.0f;
        float other_avg = (sum - d_hr) / (float) (count - 1);
        if (other_avg > 0.0001f) {
            float ratio = d_hr / other_avg;
            if (ratio < 0.0f) {
                ratio = 0.0f;
            } else if (ratio > PCM_CUSUM_RATIO_MAX) {
                ratio = PCM_CUSUM_RATIO_MAX;
            }
            state->ewma_ratio[d] = (PCM_CUSUM_ALPHA * ratio) +
                                   ((1.0f - PCM_CUSUM_ALPHA) * state->ewma_ratio[d]);
            state->cusum[d] = fmaxf(0.0f, state->cusum[d] + (drift_k - state->ewma_ratio[d]));
        }

        if (state->cusum[d] > max_s) {
            max_s = state->cusum[d];
            best_domain = (uint8_t) d;
        }
    }

    if (max_s < 0.0f) {
        max_s = 0.0f;
    }
    if (out_domain != NULL) {
        *out_domain = best_domain;
    }
    if (out_cusum != NULL) {
        *out_cusum = max_s;
    }

    return max_s >= threshold_h;
}

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
                                                   uint16_t * learned_min_vcore_mv)
{
    float dvfs_freq_off = (dvfs_freq_offset_mhz != NULL) ? *dvfs_freq_offset_mhz : 0.0f;
    int16_t dvfs_volt_off = (dvfs_voltage_offset_mv != NULL) ? *dvfs_voltage_offset_mv : 0;
    float wd_freq_off = (watchdog_freq_offset_mhz != NULL) ? *watchdog_freq_offset_mhz : 0.0f;

    if (profile == PCM_DVFS_PROFILE_BALANCED) {
        dvfs_volt_off = 0;
    }

    uint16_t cur_base_mv = pcm_compute_effective_vcore(target_voltage_mv, dvfs_volt_off, 0, 0);
    uint16_t effective_vcore_mv = pcm_compute_effective_vcore(target_voltage_mv, dvfs_volt_off, 0, vdroop_offset_mv);

    bool profile_allows_vcore = (profile == PCM_DVFS_PROFILE_EFFICIENCY ||
                                 profile == PCM_DVFS_PROFILE_PERFORMANCE);
    bool power_ok = !isnan(power_w) && (power_w >= 0.0f) && (power_w < PCM_PRE_WATCHDOG_MAX_POWER_W);
    bool vr_ok = !isnan(vr_temp_c) && (vr_temp_c >= 0.0f) && (vr_temp_c < PCM_PRE_WATCHDOG_MAX_VR_TEMP_C);
    bool vcore_ok = (effective_vcore_mv <= (PCM_VCORE_MAX_MV - PCM_PRE_WATCHDOG_VCORE_STEP_MV)) &&
                    (cur_base_mv <= (PCM_VCORE_MAX_MV - PCM_PRE_WATCHDOG_VCORE_STEP_MV));

    if (profile_allows_vcore && power_ok && vr_ok && vcore_ok) {
        uint16_t next_base_mv = pcm_clamp_vcore_mv((int32_t) cur_base_mv + PCM_PRE_WATCHDOG_VCORE_STEP_MV);
        dvfs_volt_off = (int16_t) ((int32_t) next_base_mv - (int32_t) target_voltage_mv);
        lock_learned_min_vcore(learned_min_vcore_mv, next_base_mv);
        if (dvfs_voltage_offset_mv != NULL) {
            *dvfs_voltage_offset_mv = dvfs_volt_off;
        }
        return PCM_PRE_WATCHDOG_ACTION_VCORE_STEP_UP;
    }

    float cur_base_freq = pcm_compute_dvfs_base_freq(profile, target_frequency_mhz, dvfs_freq_off);
    float prev_eff_freq = pcm_compute_effective_freq(profile, target_frequency_mhz, dvfs_freq_off, 0.0f, wd_freq_off);

    if (profile == PCM_DVFS_PROFILE_BALANCED || profile == PCM_DVFS_PROFILE_PERFORMANCE) {
        float min_freq = PCM_FREQ_MIN_MHZ;
        float max_freq = PCM_DVFS_PERFORMANCE_MAX_FREQ_MHZ;
        pcm_get_dvfs_freq_bounds(profile, &min_freq, &max_freq);

        if (cur_base_freq > (min_freq + 0.001f) && wd_freq_off <= 0.001f) {
            float next_freq = fmaxf(min_freq, cur_base_freq - PCM_DVFS_FREQ_STEP_MHZ);
            dvfs_freq_off = next_freq - target_frequency_mhz;
            lock_learned_max_freq(learned_max_freq_mhz, next_freq);
        } else {
            lock_learned_max_freq(learned_max_freq_mhz, min_freq);
            step_watchdog_freq_offset(cur_base_freq, &wd_freq_off);
        }
    } else {
        step_watchdog_freq_offset(cur_base_freq, &wd_freq_off);
    }

    if (dvfs_freq_offset_mhz != NULL) {
        *dvfs_freq_offset_mhz = dvfs_freq_off;
    }
    if (dvfs_voltage_offset_mv != NULL) {
        *dvfs_voltage_offset_mv = dvfs_volt_off;
    }
    if (watchdog_freq_offset_mhz != NULL) {
        *watchdog_freq_offset_mhz = wd_freq_off;
    }

    float next_eff_freq = pcm_compute_effective_freq(profile, target_frequency_mhz, dvfs_freq_off, 0.0f, wd_freq_off);
    if (next_eff_freq < (prev_eff_freq - 0.001f)) {
        return PCM_PRE_WATCHDOG_ACTION_FREQ_STEP_DOWN;
    }
    return PCM_PRE_WATCHDOG_ACTION_NONE;
}
