#include <math.h>
#include "unity.h"

#include "power_control_math.h"

TEST_CASE("VCore clamping enforces 1000 to 1250 mV bounds", "[power_control]")
{
    TEST_ASSERT_EQUAL_UINT16(1000, pcm_clamp_vcore_mv(0));
    TEST_ASSERT_EQUAL_UINT16(1000, pcm_clamp_vcore_mv(950));
    TEST_ASSERT_EQUAL_UINT16(1000, pcm_clamp_vcore_mv(999));
    TEST_ASSERT_EQUAL_UINT16(1000, pcm_clamp_vcore_mv(1000));
    TEST_ASSERT_EQUAL_UINT16(1150, pcm_clamp_vcore_mv(1150));
    TEST_ASSERT_EQUAL_UINT16(1250, pcm_clamp_vcore_mv(1250));
    TEST_ASSERT_EQUAL_UINT16(1250, pcm_clamp_vcore_mv(1251));
    TEST_ASSERT_EQUAL_UINT16(1250, pcm_clamp_vcore_mv(1400));

    // Nominal with DVFS + Vdroop compensation
    TEST_ASSERT_EQUAL_UINT16(1165, pcm_compute_effective_vcore(1150, 10, 5, 10));
    // Thermal offset clamps down to minimum 1000 mV floor
    TEST_ASSERT_EQUAL_UINT16(1000, pcm_compute_effective_vcore(1050, -30, 80, 0));
    // Vdroop + DVFS positive offset never exceeds 1250 mV hard cap
    TEST_ASSERT_EQUAL_UINT16(1250, pcm_compute_effective_vcore(1220, 25, 0, 20));
    // DVFS target clamping applies before thermal subtraction
    TEST_ASSERT_EQUAL_UINT16(1240, pcm_compute_effective_vcore(1240, 30, 10, 0));
}

TEST_CASE("Effective frequency enforces 400 MHz floor and DVFS profile envelopes", "[power_control]")
{
    float min_freq = 0.0f;
    float max_freq = 0.0f;

    // Manual (0) and unknown profile have no DVFS envelope
    TEST_ASSERT_FALSE(pcm_get_dvfs_freq_bounds(PCM_DVFS_PROFILE_MANUAL, &min_freq, &max_freq));
    TEST_ASSERT_FALSE(pcm_get_dvfs_freq_bounds(99, &min_freq, &max_freq));
    TEST_ASSERT_FALSE(pcm_get_dvfs_freq_bounds(PCM_DVFS_PROFILE_EFFICIENCY, NULL, &max_freq));
    TEST_ASSERT_FALSE(pcm_get_dvfs_freq_bounds(PCM_DVFS_PROFILE_EFFICIENCY, &min_freq, NULL));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 475.0f, pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_MANUAL, 475.0f, 50.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 700.0f, pcm_compute_dvfs_base_freq(99, 700.0f, 0.0f));

    // Profile 1 (Efficiency): [500, 600] MHz
    TEST_ASSERT_TRUE(pcm_get_dvfs_freq_bounds(PCM_DVFS_PROFILE_EFFICIENCY, &min_freq, &max_freq));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 500.0f, min_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 600.0f, max_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 500.0f, pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_EFFICIENCY, 485.0f, -25.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f, pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_EFFICIENCY, 525.0f, 25.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 600.0f, pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_EFFICIENCY, 575.0f, 50.0f));

    // Profile 2 (Balanced): [525, 625] MHz
    TEST_ASSERT_TRUE(pcm_get_dvfs_freq_bounds(PCM_DVFS_PROFILE_BALANCED, &min_freq, &max_freq));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 525.0f, min_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 625.0f, max_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 525.0f, pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_BALANCED, 500.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 625.0f, pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_BALANCED, 600.0f, 50.0f));

    // Profile 3 (Performance): [550, 675] MHz
    TEST_ASSERT_TRUE(pcm_get_dvfs_freq_bounds(PCM_DVFS_PROFILE_PERFORMANCE, &min_freq, &max_freq));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f, min_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 675.0f, max_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f, pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_PERFORMANCE, 500.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 675.0f, pcm_compute_dvfs_base_freq(PCM_DVFS_PROFILE_PERFORMANCE, 650.0f, 50.0f));

    // Effective frequency applies thermal and watchdog offsets with 400.0 MHz floor
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 500.0f, pcm_compute_effective_freq(PCM_DVFS_PROFILE_BALANCED, 550.0f, 0.0f, 25.0f, 25.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 407.5f, pcm_compute_effective_freq(PCM_DVFS_PROFILE_MANUAL, 420.0f, 0.0f, 12.5f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 400.0f, pcm_compute_effective_freq(PCM_DVFS_PROFILE_PERFORMANCE, 550.0f, 0.0f, 100.0f, 80.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 400.0f, pcm_compute_effective_freq(PCM_DVFS_PROFILE_MANUAL, 350.0f, 0.0f, 0.0f, 0.0f));
}

TEST_CASE("Vdroop slow-loop steps by 5 mV up to 20 mV and blocks above 32 W", "[power_control]")
{
    uint16_t offset = 0;

    // Invalid or low measured voltage leaves offset untouched
    TEST_ASSERT_EQUAL_UINT16(10, pcm_step_vdroop_offset_mv(10, 1150, NAN, 20.0f));
    TEST_ASSERT_EQUAL_UINT16(10, pcm_step_vdroop_offset_mv(10, 1150, 500.0f, 20.0f));

    // Inside +/- 5 mV deadband around 1150 mV: no adjustment
    TEST_ASSERT_EQUAL_UINT16(0, pcm_step_vdroop_offset_mv(0, 1150, 1145.0f, 25.0f));
    TEST_ASSERT_EQUAL_UINT16(5, pcm_step_vdroop_offset_mv(5, 1150, 1150.0f, 25.0f));
    TEST_ASSERT_EQUAL_UINT16(5, pcm_step_vdroop_offset_mv(5, 1150, 1155.0f, 25.0f));

    // Progressive +5 mV steps up to +20 mV cap when power <= 32.0 W
    offset = pcm_step_vdroop_offset_mv(offset, 1150, 1140.0f, 28.0f);
    TEST_ASSERT_EQUAL_UINT16(5, offset);
    offset = pcm_step_vdroop_offset_mv(offset, 1150, 1140.0f, 29.0f);
    TEST_ASSERT_EQUAL_UINT16(10, offset);
    offset = pcm_step_vdroop_offset_mv(offset, 1150, 1140.0f, 30.0f);
    TEST_ASSERT_EQUAL_UINT16(15, offset);
    offset = pcm_step_vdroop_offset_mv(offset, 1150, 1140.0f, 32.0f);
    TEST_ASSERT_EQUAL_UINT16(20, offset);
    offset = pcm_step_vdroop_offset_mv(offset, 1150, 1140.0f, 25.0f);
    TEST_ASSERT_EQUAL_UINT16(20, offset);

    // Power above 32.0 W or NaN blocks positive steps
    TEST_ASSERT_EQUAL_UINT16(5, pcm_step_vdroop_offset_mv(5, 1150, 1140.0f, 32.1f));
    TEST_ASSERT_EQUAL_UINT16(5, pcm_step_vdroop_offset_mv(5, 1150, 1140.0f, NAN));

    // 1250 mV ceiling headroom clamp (base 1240 mV => max vdroop offset 10 mV)
    TEST_ASSERT_EQUAL_UINT16(5, pcm_step_vdroop_offset_mv(0, 1240, 1230.0f, 25.0f));
    TEST_ASSERT_EQUAL_UINT16(10, pcm_step_vdroop_offset_mv(5, 1240, 1230.0f, 25.0f));
    TEST_ASSERT_EQUAL_UINT16(10, pcm_step_vdroop_offset_mv(10, 1240, 1230.0f, 25.0f));
    TEST_ASSERT_EQUAL_UINT16(10, pcm_step_vdroop_offset_mv(20, 1240, 1240.0f, 25.0f));

    // Measured voltage above base + 5 mV steps down in 5 mV decrements down to 0
    TEST_ASSERT_EQUAL_UINT16(15, pcm_step_vdroop_offset_mv(20, 1150, 1156.0f, 25.0f));
    TEST_ASSERT_EQUAL_UINT16(0, pcm_step_vdroop_offset_mv(3, 1150, 1156.0f, 25.0f));
    TEST_ASSERT_EQUAL_UINT16(0, pcm_step_vdroop_offset_mv(0, 1150, 1160.0f, 25.0f));
}

TEST_CASE("EMC2101 tachometer conversion guards 0x0000, 0xFFFF, and saturated RPM", "[power_control][emc2101]")
{
    // Invalid / stalled readings return 0 without division-by-zero panic
    TEST_ASSERT_EQUAL_UINT16(0, pcm_emc2101_tach_to_rpm(0x0000));
    TEST_ASSERT_EQUAL_UINT16(0, pcm_emc2101_tach_to_rpm(0xFFFF));

    // Saturated counts yielding RPM == 82 are filtered to 0
    TEST_ASSERT_EQUAL_UINT16(0, pcm_emc2101_tach_to_rpm(0xFFFE));
    TEST_ASSERT_EQUAL_UINT16(0, pcm_emc2101_tach_to_rpm(65061));

    // Valid readings convert via 5400000 / reading
    TEST_ASSERT_EQUAL_UINT16(83, pcm_emc2101_tach_to_rpm(65060));
    TEST_ASSERT_EQUAL_UINT16(3000, pcm_emc2101_tach_to_rpm(1800));
    TEST_ASSERT_EQUAL_UINT16(5000, pcm_emc2101_tach_to_rpm(1080));
    TEST_ASSERT_EQUAL_UINT16(10000, pcm_emc2101_tach_to_rpm(540));

    // Extremely small non-zero glitch clamps to UINT16_MAX instead of wrapping
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, pcm_emc2101_tach_to_rpm(1));
}

TEST_CASE("PID effective temperature combines ASIC max and VR - 22C offset", "[power_control][thermal]")
{
    // ASIC dominant when VR is cool (74.0 - 22.0 = 52.0 < 60.0)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, pcm_compute_pid_effective_temp(60.0f, -1.0f, 74.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 62.5f, pcm_compute_pid_effective_temp(59.0f, 62.5f, 80.0f));

    // VR dominant when VR heats up near 90 C threshold (86.0 - 22.0 = 64.0 > 58.0)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 64.0f, pcm_compute_pid_effective_temp(58.0f, -1.0f, 86.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 68.0f, pcm_compute_pid_effective_temp(60.0f, 61.0f, 90.0f));

    // vr_temp <= 0 or NaN (boards without TPS546 or sensor unavailable) leaves ASIC temperature unperturbed
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, pcm_compute_pid_effective_temp(60.0f, -1.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, pcm_compute_pid_effective_temp(60.0f, 0.0f, -1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 60.0f, pcm_compute_pid_effective_temp(60.0f, NAN, NAN));

    // Invalid chip1/chip2 falls back to valid chip or VR equivalent (> 0), or 0.0f when all invalid
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 57.0f, pcm_compute_pid_effective_temp(-1.0f, 57.0f, 0.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 64.0f, pcm_compute_pid_effective_temp(-1.0f, NAN, 86.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pcm_compute_pid_effective_temp(-1.0f, 0.0f, 20.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, pcm_compute_pid_effective_temp(-1.0f, NAN, 0.0f));
}

TEST_CASE("DVFS Efficiency profile locks learned VCore floor and elevates on watchdog", "[power_control][dvfs]")
{
    int16_t volt_offset = 0;
    uint16_t learned_min_vcore = 0;
    float dvfs_freq_offset = 0.0f;
    float watchdog_freq_offset = 0.0f;
    float learned_max_freq = 0.0f;

    // Low error steps VCore down by 5 mV per 60s cycle: 1150 -> 1145 -> 1140 -> 1135
    TEST_ASSERT_EQUAL_UINT16(1145, pcm_dvfs_step_efficiency(1150, &volt_offset, &learned_min_vcore, 0.2f, 22.0f));
    TEST_ASSERT_EQUAL_INT16(-5, volt_offset);
    TEST_ASSERT_EQUAL_UINT16(0, learned_min_vcore);

    TEST_ASSERT_EQUAL_UINT16(1140, pcm_dvfs_step_efficiency(1150, &volt_offset, &learned_min_vcore, 0.5f, 22.0f));
    TEST_ASSERT_EQUAL_UINT16(1135, pcm_dvfs_step_efficiency(1150, &volt_offset, &learned_min_vcore, 0.1f, 22.0f));
    TEST_ASSERT_EQUAL_INT16(-15, volt_offset);

    // Exceeding 0.5% error with power <= 32.0 W steps up +10 mV and locks learned floor at 1145 mV
    TEST_ASSERT_EQUAL_UINT16(1145, pcm_dvfs_step_efficiency(1150, &volt_offset, &learned_min_vcore, 0.6f, 23.0f));
    TEST_ASSERT_EQUAL_INT16(-5, volt_offset);
    TEST_ASSERT_EQUAL_UINT16(1145, learned_min_vcore);
    TEST_ASSERT_EQUAL_UINT16(1145, pcm_dvfs_effective_vcore_floor(learned_min_vcore));

    // Subsequent low-error cycles cannot drop below learned floor (eliminates sawtooth hunting)
    TEST_ASSERT_EQUAL_UINT16(1145, pcm_dvfs_step_efficiency(1150, &volt_offset, &learned_min_vcore, 0.0f, 22.0f));
    TEST_ASSERT_EQUAL_INT16(-5, volt_offset);

    // High power (> 32.0 W) blocks further VCore increase even when error > 0.5%
    TEST_ASSERT_EQUAL_UINT16(1145, pcm_dvfs_step_efficiency(1150, &volt_offset, &learned_min_vcore, 0.9f, 32.1f));
    TEST_ASSERT_EQUAL_UINT16(1145, learned_min_vcore);

    // Watchdog Level 2 in Efficiency elevates learned floor by +10 mV and steps down watchdog freq
    pcm_dvfs_apply_watchdog_event(PCM_DVFS_PROFILE_EFFICIENCY, 2, 550.0f, 1150,
                                  &dvfs_freq_offset, &volt_offset, &watchdog_freq_offset,
                                  &learned_max_freq, &learned_min_vcore);
    TEST_ASSERT_EQUAL_UINT16(1155, learned_min_vcore);
    TEST_ASSERT_EQUAL_INT16(5, volt_offset);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, watchdog_freq_offset);
}

TEST_CASE("DVFS Balanced keeps fixed NVS voltage, learns ceiling, and pulls back thermally", "[power_control][dvfs]")
{
    float freq_offset = 0.0f;
    int16_t volt_offset = 15; // Must be forced back to 0 in Balanced mode
    float learned_max_freq = 0.0f;

    // Scales up +12.5 MHz while cool and low error, forcing dvfs_voltage_offset_mv == 0
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 562.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.3f, 60.0f, 72.0f, 25.0f));
    TEST_ASSERT_EQUAL_INT16(0, volt_offset);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, freq_offset);

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 575.0f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.3f, 61.0f, 74.0f, 26.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.4f, 62.0f, 75.0f, 27.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 600.0f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.5f, 63.0f, 76.0f, 28.0f));

    // Error > 0.8% steps down -12.5 MHz to 587.5 MHz and locks learned ceiling
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.9f, 62.0f, 76.0f, 28.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f, learned_max_freq);

    // Subsequent cool/low-error cycle stays capped at learned ceiling 587.5 MHz
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 60.0f, 72.0f, 26.0f));

    // Deadband between 64.0 and 65.5 C holds steady without climbing or pulling back
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 64.5f, 78.0f, 27.0f));

    // Preventive thermal pullback (ASIC >= 65.5 C) reduces -12.5 MHz WITHOUT lowering learned ceiling
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 575.0f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 65.5f, 79.0f, 27.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f, learned_max_freq);

    // Preventive thermal pullback on VR >= 84.0 C continues toward target_frequency (550 MHz)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 562.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 63.0f, 84.0f, 27.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 66.0f, 85.0f, 27.0f));
    // Does not pull back below target_frequency (550 MHz)
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 66.0f, 85.0f, 27.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f, learned_max_freq);

    // Once cool again (< 64.0 C ASIC and < 80.0 C VR), recovers gradually up to learned ceiling
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 562.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 60.0f, 75.0f, 26.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 575.0f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 60.0f, 75.0f, 26.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 60.0f, 75.0f, 26.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f,
                             pcm_dvfs_step_balanced(550.0f, &freq_offset, &volt_offset, &learned_max_freq,
                                                    0.2f, 60.0f, 75.0f, 26.0f));
}

TEST_CASE("DVFS Performance applies 2D silicon learning and preventive thermal/power pullback", "[power_control][dvfs]")
{
    float freq_offset = 0.0f;
    int16_t volt_offset = 0;
    float learned_max_freq = 0.0f;
    uint16_t learned_min_vcore = 0;

    // Start at 600 MHz / 1230 mV -> scale up to 625 MHz
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 612.5f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       0.5f, 62.0f, 78.0f, 28.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 625.0f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       0.5f, 62.0f, 78.0f, 28.5f));

    // Error > 1.0% while VCore < 1250 mV and power < 31.0 W steps VCore +10 mV and locks floor
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 625.0f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       1.2f, 63.0f, 79.0f, 29.0f));
    TEST_ASSERT_EQUAL_INT16(10, volt_offset);
    TEST_ASSERT_EQUAL_UINT16(1240, learned_min_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, learned_max_freq);

    // Another error > 1.0% reaches 1250 mV cap
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 625.0f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       1.3f, 63.0f, 79.0f, 29.5f));
    TEST_ASSERT_EQUAL_INT16(20, volt_offset);
    TEST_ASSERT_EQUAL_UINT16(1250, learned_min_vcore);

    // Error > 1.0% at 1250 mV steps frequency down -12.5 MHz and locks learned ceiling at 612.5 MHz
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 612.5f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       1.4f, 63.0f, 79.0f, 30.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 612.5f, learned_max_freq);

    // Preventive pullback at power >= 31.2 W reduces -12.5 MHz without modifying learned ceiling
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 600.0f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       0.4f, 64.0f, 80.0f, 31.2f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 612.5f, learned_max_freq);

    // Cool and power < 31.0 W recovers back to learned_max_freq (612.5 MHz) and stops there
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 612.5f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       0.4f, 62.0f, 78.0f, 29.5f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 612.5f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       0.4f, 62.0f, 78.0f, 29.5f));

    // Preventive thermal pullback on ASIC >= 66.5 C or VR >= 87.0 C also works without locking ceiling
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 600.0f,
                             pcm_dvfs_step_performance(600.0f, 1230, &freq_offset, &volt_offset,
                                                       &learned_max_freq, &learned_min_vcore,
                                                       0.4f, 66.5f, 80.0f, 30.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 612.5f, learned_max_freq);
}

TEST_CASE("Watchdog Level 1/2 coordinates with DVFS learned limits and reset restores full bounds", "[power_control][dvfs][watchdog]")
{
    float dvfs_freq_offset = 50.0f; // target 550 + 50 = 600 MHz
    int16_t dvfs_volt_offset = 0;
    float watchdog_freq_offset = 0.0f;
    float learned_max_freq = 0.0f;
    uint16_t learned_min_vcore = 0;

    // Watchdog Level 1 in Balanced keeps current frequency and locks learned ceiling at 600 MHz
    pcm_dvfs_apply_watchdog_event(PCM_DVFS_PROFILE_BALANCED, 1, 550.0f, 1150,
                                  &dvfs_freq_offset, &dvfs_volt_offset, &watchdog_freq_offset,
                                  &learned_max_freq, &learned_min_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 600.0f, learned_max_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 50.0f, dvfs_freq_offset);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, watchdog_freq_offset);

    // Watchdog Level 2 in Balanced above profile min steps down dvfs_freq_offset by 12.5 MHz and locks ceiling
    pcm_dvfs_apply_watchdog_event(PCM_DVFS_PROFILE_BALANCED, 2, 550.0f, 1150,
                                  &dvfs_freq_offset, &dvfs_volt_offset, &watchdog_freq_offset,
                                  &learned_max_freq, &learned_min_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 37.5f, dvfs_freq_offset);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f, learned_max_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, watchdog_freq_offset);

    // Subsequent 60s cool DVFS cycle does not undo Watchdog reduction
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 587.5f,
                             pcm_dvfs_step_balanced(550.0f, &dvfs_freq_offset, &dvfs_volt_offset,
                                                    &learned_max_freq, 0.1f, 58.0f, 70.0f, 25.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 37.5f, dvfs_freq_offset);

    // Watchdog Level 2 at profile min (525.0 MHz) preserves profile floor and drops via watchdog_freq_offset
    dvfs_freq_offset = -25.0f; // 550 - 25 = 525 MHz
    learned_max_freq = 0.0f;
    watchdog_freq_offset = 0.0f;
    pcm_dvfs_apply_watchdog_event(PCM_DVFS_PROFILE_BALANCED, 2, 550.0f, 1150,
                                  &dvfs_freq_offset, &dvfs_volt_offset, &watchdog_freq_offset,
                                  &learned_max_freq, &learned_min_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 525.0f, learned_max_freq);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, watchdog_freq_offset);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 512.5f,
                             pcm_compute_effective_freq(PCM_DVFS_PROFILE_BALANCED, 550.0f,
                                                        dvfs_freq_offset, 0.0f, watchdog_freq_offset));

    // Watchdog Level 1 and Level 2 in Performance
    dvfs_freq_offset = 25.0f; // 600 + 25 = 625 MHz
    watchdog_freq_offset = 0.0f;
    learned_max_freq = 0.0f;
    pcm_dvfs_apply_watchdog_event(PCM_DVFS_PROFILE_PERFORMANCE, 1, 600.0f, 1200,
                                  &dvfs_freq_offset, &dvfs_volt_offset, &watchdog_freq_offset,
                                  &learned_max_freq, &learned_min_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 625.0f, learned_max_freq);
    pcm_dvfs_apply_watchdog_event(PCM_DVFS_PROFILE_PERFORMANCE, 2, 600.0f, 1200,
                                  &dvfs_freq_offset, &dvfs_volt_offset, &watchdog_freq_offset,
                                  &learned_max_freq, &learned_min_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.5f, dvfs_freq_offset);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 612.5f, learned_max_freq);

    // Resetting learned bounds restores full profile ceiling / floor
    learned_max_freq = 0.0f;
    learned_min_vcore = 0;
    TEST_ASSERT_FLOAT_WITHIN(0.001f, PCM_DVFS_PERFORMANCE_MAX_FREQ_MHZ,
                             pcm_dvfs_effective_freq_ceiling(PCM_DVFS_PERFORMANCE_MIN_FREQ_MHZ,
                                                             PCM_DVFS_PERFORMANCE_MAX_FREQ_MHZ,
                                                             learned_max_freq));
    TEST_ASSERT_EQUAL_UINT16(PCM_VCORE_MIN_MV, pcm_dvfs_effective_vcore_floor(learned_min_vcore));
}
