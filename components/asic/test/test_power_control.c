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
