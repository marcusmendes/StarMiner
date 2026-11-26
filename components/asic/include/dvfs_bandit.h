#ifndef DVFS_BANDIT_H
#define DVFS_BANDIT_H

#include <stdbool.h>
#include <stdint.h>
#include "power_control_math.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DVFS_BANDIT_FREQ_STEPS              15
#define DVFS_BANDIT_VCORE_STEPS             21
#define DVFS_BANDIT_GRID_CELLS              (DVFS_BANDIT_FREQ_STEPS * DVFS_BANDIT_VCORE_STEPS)

#define DVFS_BANDIT_FREQ_MIN_MHZ            500.0f
#define DVFS_BANDIT_FREQ_MAX_MHZ            675.0f
#define DVFS_BANDIT_FREQ_STEP_MHZ           12.5f

#define DVFS_BANDIT_VCORE_MIN_MV            1050
#define DVFS_BANDIT_VCORE_MAX_MV            1250
#define DVFS_BANDIT_VCORE_STEP_MV           10

#define DVFS_BANDIT_NORM_HASHRATE_GHS       1200.0f
#define DVFS_BANDIT_ALPHA_ERR               0.25f
#define DVFS_BANDIT_BETA_POWER              0.08f
#define DVFS_BANDIT_GAMMA_CHIP_TEMP         0.06f
#define DVFS_BANDIT_DELTA_VR_TEMP           0.05f
#define DVFS_BANDIT_FAULT_PENALTY               1.50f
#define DVFS_BANDIT_EFF_POWER_BASE_W            18.0f
#define DVFS_BANDIT_EFF_POWER_SLOPE             0.008f
#define DVFS_BANDIT_STALL_HASHRATE_RATIO        0.70f
#define DVFS_BANDIT_CLEAN_ERR_NO_VCORE_UP_PCT   0.30f
#define DVFS_BANDIT_VISITED_WEIGHT_FLOOR        0.20f
#define DVFS_BANDIT_UNVISITED_PSEUDO_N          1.2f
#define DVFS_BANDIT_SWITCH_MARGIN               0.004f
#define DVFS_BANDIT_UCB_C                       0.015f

#define DVFS_BANDIT_DISCOUNT_GAMMA              0.995f
#define DVFS_BANDIT_DRIFT_DECAY_GAMMA           0.85f
#define DVFS_BANDIT_DRIFT_TEMP_THRESH_C         2.5f
#define DVFS_BANDIT_DRIFT_ERR_THRESH_PCT        1.5f

#define DVFS_BANDIT_MIN_DWELL_STEPS             2
#define DVFS_BANDIT_SAFE_STABLE_STEPS           2
#define DVFS_BANDIT_BLACKLIST_STEPS             10
#define DVFS_BANDIT_RECOVERY_COOLDOWN_STEPS     2

typedef struct {
    float weight_n;
    float reward_sum;
    float avg_heff_ghs;
    uint32_t last_visited_step;
    uint32_t blacklist_until_step;
    uint16_t fault_count;
    uint16_t clean_streak;
    uint16_t visit_count;
} DvfsBanditCell;

typedef struct {
    DvfsBanditCell cells[DVFS_BANDIT_FREQ_STEPS][DVFS_BANDIT_VCORE_STEPS];
    uint8_t cur_freq_idx;
    uint8_t cur_vcore_idx;
    uint16_t dwell_steps;
    uint32_t total_steps;
    uint16_t recovery_cooldown_steps;
    float baseline_freq_mhz;
    uint16_t baseline_vcore_mv;
    float best_safe_freq_mhz;
    uint16_t best_safe_vcore_mv;
    float best_safe_heff_ghs;
    float ewma_chip_temp_c;
    float ewma_err_pct;
    bool ewma_initialized;
} DvfsBanditState;

typedef struct {
    float hashrate_ghs;
    float expected_hashrate_ghs;
    float error_percentage;
    float power_w;
    float chip_temp_c;
    float vr_temp_c;
} DvfsBanditSample;

/**
 * @brief Maps an ASIC frequency in MHz onto the discrete 15-point grid [500.0, 675.0].
 */
uint8_t dvfs_bandit_freq_to_idx(float freq_mhz);

/**
 * @brief Converts a frequency grid index [0..14] to MHz.
 */
float dvfs_bandit_idx_to_freq(uint8_t freq_idx);

/**
 * @brief Maps an ASIC core voltage in mV onto the discrete 21-point grid [1050, 1250].
 */
uint8_t dvfs_bandit_vcore_to_idx(uint16_t vcore_mv);

/**
 * @brief Converts a VCore grid index [0..20] to mV.
 */
uint16_t dvfs_bandit_idx_to_vcore(uint8_t vcore_idx);

/**
 * @brief Computes effective useful hashrate H_eff = hashrate_ghs * (1 - error_percentage / 100).
 */
float dvfs_bandit_compute_effective_hashrate(float hashrate_ghs, float error_percentage);

/**
 * @brief Computes normalized multi-objective reward from H_eff and error/power/thermal/fault penalties.
 */
float dvfs_bandit_compute_reward(const DvfsBanditSample * sample, bool fault_occurred);

/**
 * @brief Initializes 2D Bandit state around target or restored safe operating point.
 */
void dvfs_bandit_init(DvfsBanditState * state,
                      float target_freq_mhz,
                      uint16_t target_vcore_mv,
                      float restored_best_freq_mhz,
                      uint16_t restored_best_vcore_mv);

/**
 * @brief Multiplies discounted cell weights and reward sums by gamma in [0, 1].
 */
void dvfs_bandit_apply_decay(DvfsBanditState * state, float gamma);

/**
 * @brief Evaluates one >=60s control window, updates UCB stats, and selects next safe +/-1 grid point.
 */
void dvfs_bandit_step(DvfsBanditState * state,
                      const DvfsBanditSample * sample,
                      float * out_freq_mhz,
                      uint16_t * out_vcore_mv,
                      float * out_best_freq_mhz,
                      uint16_t * out_best_vcore_mv);

/**
 * @brief Records a pre-watchdog or hard-watchdog fault on the active cell and rolls back to a safe point.
 */
void dvfs_bandit_record_fault_and_rollback(DvfsBanditState * state,
                                           bool is_hard_watchdog,
                                           float * out_freq_mhz,
                                           uint16_t * out_vcore_mv);

#ifdef __cplusplus
}
#endif

#endif /* DVFS_BANDIT_H */
