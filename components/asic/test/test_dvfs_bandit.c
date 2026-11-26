#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "unity.h"

#include "dvfs_bandit.h"

static DvfsBanditState s_bandit_test_state;

TEST_CASE("Bandit grid indexing enforces 500-675 MHz and 1050-1250 mV bounds", "[dvfs_bandit]")
{
    TEST_ASSERT_EQUAL_UINT8(0, dvfs_bandit_freq_to_idx(400.0f));
    TEST_ASSERT_EQUAL_UINT8(0, dvfs_bandit_freq_to_idx(500.0f));
    TEST_ASSERT_EQUAL_UINT8(4, dvfs_bandit_freq_to_idx(550.0f));
    TEST_ASSERT_EQUAL_UINT8(14, dvfs_bandit_freq_to_idx(675.0f));
    TEST_ASSERT_EQUAL_UINT8(14, dvfs_bandit_freq_to_idx(750.0f));

    TEST_ASSERT_FLOAT_WITHIN(0.001f, 500.0f, dvfs_bandit_idx_to_freq(0));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f, dvfs_bandit_idx_to_freq(4));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 675.0f, dvfs_bandit_idx_to_freq(14));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 675.0f, dvfs_bandit_idx_to_freq(99));

    TEST_ASSERT_EQUAL_UINT8(0, dvfs_bandit_vcore_to_idx(950));
    TEST_ASSERT_EQUAL_UINT8(0, dvfs_bandit_vcore_to_idx(1050));
    TEST_ASSERT_EQUAL_UINT8(10, dvfs_bandit_vcore_to_idx(1150));
    TEST_ASSERT_EQUAL_UINT8(20, dvfs_bandit_vcore_to_idx(1250));
    TEST_ASSERT_EQUAL_UINT8(20, dvfs_bandit_vcore_to_idx(1400));

    TEST_ASSERT_EQUAL_UINT16(1050, dvfs_bandit_idx_to_vcore(0));
    TEST_ASSERT_EQUAL_UINT16(1150, dvfs_bandit_idx_to_vcore(10));
    TEST_ASSERT_EQUAL_UINT16(1250, dvfs_bandit_idx_to_vcore(20));
    TEST_ASSERT_EQUAL_UINT16(1250, dvfs_bandit_idx_to_vcore(99));
}

TEST_CASE("Bandit effective hashrate and multi-objective reward penalize errors, power, temps, and faults", "[dvfs_bandit]")
{
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 1188.0f, dvfs_bandit_compute_effective_hashrate(1200.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, dvfs_bandit_compute_effective_hashrate(0.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, dvfs_bandit_compute_effective_hashrate(NAN, 0.5f));

    DvfsBanditSample clean = {
        .hashrate_ghs = 1200.0f,
        .error_percentage = 0.2f,
        .power_w = 24.0f,
        .chip_temp_c = 60.0f,
        .vr_temp_c = 70.0f,
    };
    float r_clean = dvfs_bandit_compute_reward(&clean, false);
    float expected_clean = (1197.6f / 1200.0f) - (DVFS_BANDIT_EFF_POWER_SLOPE * (24.0f - DVFS_BANDIT_EFF_POWER_BASE_W));
    TEST_ASSERT_FLOAT_WITHIN(0.001f, expected_clean, r_clean);

    DvfsBanditSample stressed = {
        .hashrate_ghs = 1200.0f,
        .error_percentage = 1.5f,
        .power_w = 30.0f,
        .chip_temp_c = 65.0f,
        .vr_temp_c = 82.0f,
    };
    float r_stressed = dvfs_bandit_compute_reward(&stressed, false);
    float r_fault = dvfs_bandit_compute_reward(&stressed, true);
    TEST_ASSERT_TRUE(r_stressed < r_clean);
    TEST_ASSERT_TRUE(r_fault < r_stressed);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, r_stressed - DVFS_BANDIT_FAULT_PENALTY, r_fault);
}

TEST_CASE("Bandit enforces dwell hysteresis, +/-1 grid steps, and safe point promotion", "[dvfs_bandit]")
{
    dvfs_bandit_init(&s_bandit_test_state, 550.0f, 1150, 0.0f, 0);

    float out_freq = 0.0f;
    uint16_t out_vcore = 0;
    float best_freq = 0.0f;
    uint16_t best_vcore = 0;

    DvfsBanditSample nominal = {
        .hashrate_ghs = 1150.0f,
        .error_percentage = 0.1f,
        .power_w = 22.0f,
        .chip_temp_c = 59.0f,
        .vr_temp_c = 68.0f,
    };

    // First step must remain at initial cell due to minimum dwell hysteresis (2 steps)
    dvfs_bandit_step(&s_bandit_test_state, &nominal, &out_freq, &out_vcore, &best_freq, &best_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f, out_freq);
    TEST_ASSERT_EQUAL_UINT16(1150, out_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.0f, best_freq);
    TEST_ASSERT_EQUAL_UINT16(0, best_vcore);

    // Second clean step promotes initial cell as proven safe point and allows +/-1 neighbor exploration
    dvfs_bandit_step(&s_bandit_test_state, &nominal, &out_freq, &out_vcore, &best_freq, &best_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f, best_freq);
    TEST_ASSERT_EQUAL_UINT16(1150, best_vcore);
    TEST_ASSERT_TRUE(fabsf(out_freq - 550.0f) <= DVFS_BANDIT_FREQ_STEP_MHZ + 0.001f);
    TEST_ASSERT_TRUE(abs((int) out_vcore - 1150) <= DVFS_BANDIT_VCORE_STEP_MV);
}

TEST_CASE("Bandit applies temporal decay on drift and rolls back on pre-watchdog/watchdog faults", "[dvfs_bandit]")
{
    dvfs_bandit_init(&s_bandit_test_state, 550.0f, 1150, 0.0f, 0);

    float out_freq = 0.0f;
    uint16_t out_vcore = 0;
    float best_freq = 0.0f;
    uint16_t best_vcore = 0;

    DvfsBanditSample good = {
        .hashrate_ghs = 1200.0f,
        .error_percentage = 0.1f,
        .power_w = 22.5f,
        .chip_temp_c = 58.0f,
        .vr_temp_c = 66.0f,
    };

    // Lock safe point at (550 MHz, 1150 mV)
    dvfs_bandit_step(&s_bandit_test_state, &good, &out_freq, &out_vcore, &best_freq, &best_vcore);
    dvfs_bandit_step(&s_bandit_test_state, &good, &out_freq, &out_vcore, &best_freq, &best_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f, best_freq);
    TEST_ASSERT_EQUAL_UINT16(1150, best_vcore);

    // Move to a higher point, then trigger fault -> rollback to safe point
    uint8_t safe_f = dvfs_bandit_freq_to_idx(550.0f);
    uint8_t safe_v = dvfs_bandit_vcore_to_idx(1150);
    s_bandit_test_state.cur_freq_idx = safe_f + 2; // 575.0 MHz
    s_bandit_test_state.cur_vcore_idx = safe_v;
    uint8_t fault_f = s_bandit_test_state.cur_freq_idx;
    uint8_t fault_v = s_bandit_test_state.cur_vcore_idx;

    dvfs_bandit_record_fault_and_rollback(&s_bandit_test_state, false, &out_freq, &out_vcore);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 550.0f, out_freq);
    TEST_ASSERT_EQUAL_UINT16(1150, out_vcore);
    TEST_ASSERT_EQUAL_UINT16(1, s_bandit_test_state.cells[fault_f][fault_v].fault_count);
    TEST_ASSERT_TRUE(s_bandit_test_state.cells[fault_f][fault_v].blacklist_until_step > s_bandit_test_state.total_steps);

    // Ambient temperature drift (>= 2.5 C jump) triggers geometric decay on historical weights
    float weight_before = s_bandit_test_state.cells[safe_f][safe_v].weight_n;
    DvfsBanditSample drifted = {
        .hashrate_ghs = 1195.0f,
        .error_percentage = 0.2f,
        .power_w = 23.0f,
        .chip_temp_c = 64.0f, // +6 C jump vs ~58 C baseline
        .vr_temp_c = 72.0f,
    };
    dvfs_bandit_step(&s_bandit_test_state, &drifted, &out_freq, &out_vcore, &best_freq, &best_vcore);
    TEST_ASSERT_TRUE(s_bandit_test_state.cells[safe_f][safe_v].weight_n < (weight_before + 1.0f) * 0.90f);
}

TEST_CASE("Bandit v2 converges upward from mid-range without downward bias to 500 MHz", "[dvfs_bandit]")
{
    dvfs_bandit_init(&s_bandit_test_state, 600.0f, 1150, 0.0f, 0);

    float out_freq = 600.0f;
    uint16_t out_vcore = 1150;
    float best_freq = 0.0f;
    uint16_t best_vcore = 0;

    for (int step = 0; step < 14; step++) {
        float noise = (step % 2 == 0) ? -120.0f : 120.0f;
        float exp_hr = out_freq * 2.04f;
        DvfsBanditSample s = {
            .hashrate_ghs = exp_hr + noise,
            .expected_hashrate_ghs = exp_hr,
            .error_percentage = 0.1f,
            .power_w = 21.0f + 0.03f * (out_freq - 600.0f),
            .chip_temp_c = 58.0f,
            .vr_temp_c = 66.0f,
        };
        dvfs_bandit_step(&s_bandit_test_state, &s, &out_freq, &out_vcore, &best_freq, &best_vcore);
        TEST_ASSERT_TRUE(out_freq >= 600.0f);
        TEST_ASSERT_TRUE(out_vcore <= 1150);
    }

    TEST_ASSERT_TRUE(out_freq >= 650.0f);
    TEST_ASSERT_TRUE(best_freq >= 625.0f);
}

TEST_CASE("Bandit v2 continuous power penalty penalizes VCore inflation below 28 W", "[dvfs_bandit]")
{
    const float powers[] = {21.0f, 22.5f, 24.0f, 26.0f, 29.0f};
    float prev_reward = 100.0f;
    for (int i = 0; i < 5; i++) {
        DvfsBanditSample s = {
            .hashrate_ghs = 1275.0f,
            .expected_hashrate_ghs = 1275.0f,
            .error_percentage = 0.1f,
            .power_w = powers[i],
            .chip_temp_c = 58.0f,
            .vr_temp_c = 66.0f,
        };
        float r = dvfs_bandit_compute_reward(&s, false);
        TEST_ASSERT_TRUE(r < prev_reward);
        prev_reward = r;
    }

    // At 675 MHz ceiling and clean error (0.1%), controller must not step VCore up from 1150 to 1160 mV
    dvfs_bandit_init(&s_bandit_test_state, 675.0f, 1150, 0.0f, 0);
    float out_freq = 675.0f;
    uint16_t out_vcore = 1150;
    float best_freq = 0.0f;
    uint16_t best_vcore = 0;

    DvfsBanditSample ceiling_clean = {
        .hashrate_ghs = 1377.0f,
        .expected_hashrate_ghs = 1377.0f,
        .error_percentage = 0.1f,
        .power_w = 23.0f,
        .chip_temp_c = 58.0f,
        .vr_temp_c = 66.0f,
    };
    dvfs_bandit_step(&s_bandit_test_state, &ceiling_clean, &out_freq, &out_vcore, &best_freq, &best_vcore);
    dvfs_bandit_step(&s_bandit_test_state, &ceiling_clean, &out_freq, &out_vcore, &best_freq, &best_vcore);
    TEST_ASSERT_TRUE(out_vcore <= 1150);
}

TEST_CASE("Bandit v2 preserves visit_count across 0.6-1.0% error ripples and decays on >=1.5% drift", "[dvfs_bandit]")
{
    dvfs_bandit_init(&s_bandit_test_state, 600.0f, 1150, 0.0f, 0);
    uint8_t start_f = dvfs_bandit_freq_to_idx(600.0f);
    uint8_t start_v = dvfs_bandit_vcore_to_idx(1150);

    float out_freq = 600.0f;
    uint16_t out_vcore = 1150;
    float best_freq = 0.0f;
    uint16_t best_vcore = 0;

    const float ripples[] = {0.6f, 1.0f, 0.5f, 0.9f};
    for (int i = 0; i < 4; i++) {
        s_bandit_test_state.cur_freq_idx = start_f;
        s_bandit_test_state.cur_vcore_idx = start_v;
        DvfsBanditSample s = {
            .hashrate_ghs = 1224.0f,
            .expected_hashrate_ghs = 1224.0f,
            .error_percentage = ripples[i],
            .power_w = 22.0f,
            .chip_temp_c = 58.0f,
            .vr_temp_c = 66.0f,
        };
        dvfs_bandit_step(&s_bandit_test_state, &s, &out_freq, &out_vcore, &best_freq, &best_vcore);
    }

    TEST_ASSERT_EQUAL_UINT16(4, s_bandit_test_state.cells[start_f][start_v].visit_count);
    // Repeated decay must preserve visited identity and floor weight_n at DVFS_BANDIT_VISITED_WEIGHT_FLOOR
    for (int k = 0; k < 25; k++) {
        dvfs_bandit_apply_decay(&s_bandit_test_state, 0.85f);
    }
    TEST_ASSERT_TRUE(s_bandit_test_state.cells[start_f][start_v].visit_count == 4);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, DVFS_BANDIT_VISITED_WEIGHT_FLOOR,
                             s_bandit_test_state.cells[start_f][start_v].weight_n);

    // Move to another cell and verify >= 1.5% error jump accelerates decay on a heavy cell
    s_bandit_test_state.cells[start_f][start_v].weight_n = 4.0f;
    s_bandit_test_state.cells[start_f][start_v].reward_sum = 4.0f;
    s_bandit_test_state.cur_freq_idx = start_f + 1;
    s_bandit_test_state.cur_vcore_idx = start_v;
    DvfsBanditSample drift_jump = {
        .hashrate_ghs = 1249.5f,
        .expected_hashrate_ghs = 1249.5f,
        .error_percentage = 2.6f, // >= 1.5% jump above ~0.75% EWMA
        .power_w = 22.5f,
        .chip_temp_c = 58.0f,
        .vr_temp_c = 66.0f,
    };
    dvfs_bandit_step(&s_bandit_test_state, &drift_jump, &out_freq, &out_vcore, &best_freq, &best_vcore);
    TEST_ASSERT_TRUE(s_bandit_test_state.cells[start_f][start_v].weight_n < 3.6f);
}

TEST_CASE("Bandit v2 uses expected_hashrate for normal variance and falls back on severe stall", "[dvfs_bandit]")
{
    DvfsBanditSample low_noise = {
        .hashrate_ghs = 1100.0f,
        .expected_hashrate_ghs = 1275.0f,
        .error_percentage = 0.1f,
        .power_w = 22.5f,
        .chip_temp_c = 58.0f,
        .vr_temp_c = 66.0f,
    };
    DvfsBanditSample high_noise = low_noise;
    high_noise.hashrate_ghs = 1420.0f;

    float r_low = dvfs_bandit_compute_reward(&low_noise, false);
    float r_high = dvfs_bandit_compute_reward(&high_noise, false);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, r_low, r_high);

    DvfsBanditSample stalled = low_noise;
    stalled.hashrate_ghs = 600.0f; // < 0.70 * 1275.0
    DvfsBanditSample legacy_stalled = stalled;
    legacy_stalled.expected_hashrate_ghs = 0.0f;

    float r_stalled = dvfs_bandit_compute_reward(&stalled, false);
    float r_legacy = dvfs_bandit_compute_reward(&legacy_stalled, false);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, r_legacy, r_stalled);
    TEST_ASSERT_TRUE(r_stalled < r_low - 0.40f);
}
