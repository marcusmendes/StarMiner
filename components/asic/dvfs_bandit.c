#include "dvfs_bandit.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

static uint8_t clamp_freq_idx(int32_t idx)
{
    if (idx < 0) {
        return 0;
    }
    if (idx >= DVFS_BANDIT_FREQ_STEPS) {
        return DVFS_BANDIT_FREQ_STEPS - 1;
    }
    return (uint8_t) idx;
}

static uint8_t clamp_vcore_idx(int32_t idx)
{
    if (idx < 0) {
        return 0;
    }
    if (idx >= DVFS_BANDIT_VCORE_STEPS) {
        return DVFS_BANDIT_VCORE_STEPS - 1;
    }
    return (uint8_t) idx;
}

uint8_t dvfs_bandit_freq_to_idx(float freq_mhz)
{
    if (isnan(freq_mhz)) {
        return 0;
    }
    float clamped = fmaxf(DVFS_BANDIT_FREQ_MIN_MHZ, fminf(DVFS_BANDIT_FREQ_MAX_MHZ, freq_mhz));
    int32_t idx = (int32_t) lroundf((clamped - DVFS_BANDIT_FREQ_MIN_MHZ) / DVFS_BANDIT_FREQ_STEP_MHZ);
    return clamp_freq_idx(idx);
}

float dvfs_bandit_idx_to_freq(uint8_t freq_idx)
{
    uint8_t idx = clamp_freq_idx((int32_t) freq_idx);
    return DVFS_BANDIT_FREQ_MIN_MHZ + ((float) idx * DVFS_BANDIT_FREQ_STEP_MHZ);
}

uint8_t dvfs_bandit_vcore_to_idx(uint16_t vcore_mv)
{
    int32_t clamped = (int32_t) vcore_mv;
    if (clamped < DVFS_BANDIT_VCORE_MIN_MV) {
        clamped = DVFS_BANDIT_VCORE_MIN_MV;
    } else if (clamped > DVFS_BANDIT_VCORE_MAX_MV) {
        clamped = DVFS_BANDIT_VCORE_MAX_MV;
    }
    int32_t idx = (clamped - DVFS_BANDIT_VCORE_MIN_MV + (DVFS_BANDIT_VCORE_STEP_MV / 2)) / DVFS_BANDIT_VCORE_STEP_MV;
    return clamp_vcore_idx(idx);
}

uint16_t dvfs_bandit_idx_to_vcore(uint8_t vcore_idx)
{
    uint8_t idx = clamp_vcore_idx((int32_t) vcore_idx);
    uint16_t raw = (uint16_t) (DVFS_BANDIT_VCORE_MIN_MV + ((uint16_t) idx * DVFS_BANDIT_VCORE_STEP_MV));
    return pcm_clamp_vcore_mv((int32_t) raw);
}

float dvfs_bandit_compute_effective_hashrate(float hashrate_ghs, float error_percentage)
{
    if (isnan(hashrate_ghs) || hashrate_ghs <= 0.0f) {
        return 0.0f;
    }
    float err_pct = (isnan(error_percentage) || error_percentage < 0.0f) ? 0.0f : error_percentage;
    if (err_pct > 100.0f) {
        err_pct = 100.0f;
    }
    return hashrate_ghs * (1.0f - (err_pct / 100.0f));
}

float dvfs_bandit_compute_reward(const DvfsBanditSample * sample, bool fault_occurred)
{
    if (sample == NULL) {
        return fault_occurred ? -DVFS_BANDIT_FAULT_PENALTY : 0.0f;
    }

    float heff = dvfs_bandit_compute_effective_hashrate(sample->hashrate_ghs, sample->error_percentage);
    float reward = heff / DVFS_BANDIT_NORM_HASHRATE_GHS;

    if (!isnan(sample->error_percentage) && sample->error_percentage > 0.5f) {
        reward -= DVFS_BANDIT_ALPHA_ERR * (sample->error_percentage - 0.5f);
    }
    if (!isnan(sample->power_w) && sample->power_w > 28.0f) {
        reward -= DVFS_BANDIT_BETA_POWER * (sample->power_w - 28.0f);
    }
    if (!isnan(sample->chip_temp_c) && sample->chip_temp_c > 62.0f) {
        reward -= DVFS_BANDIT_GAMMA_CHIP_TEMP * (sample->chip_temp_c - 62.0f);
    }
    if (!isnan(sample->vr_temp_c) && sample->vr_temp_c > 78.0f) {
        reward -= DVFS_BANDIT_DELTA_VR_TEMP * (sample->vr_temp_c - 78.0f);
    }
    if (fault_occurred) {
        reward -= DVFS_BANDIT_FAULT_PENALTY;
    }
    return reward;
}

void dvfs_bandit_init(DvfsBanditState * state,
                      float target_freq_mhz,
                      uint16_t target_vcore_mv,
                      float restored_best_freq_mhz,
                      uint16_t restored_best_vcore_mv)
{
    if (state == NULL) {
        return;
    }

    memset(state, 0, sizeof(*state));

    float safe_target_freq = isnan(target_freq_mhz) ? DVFS_BANDIT_FREQ_MIN_MHZ : target_freq_mhz;
    safe_target_freq = fmaxf(PCM_FREQ_MIN_MHZ, fminf(DVFS_BANDIT_FREQ_MAX_MHZ, safe_target_freq));
    state->baseline_freq_mhz = safe_target_freq;
    state->baseline_vcore_mv = pcm_clamp_vcore_mv((int32_t) target_vcore_mv);

    if (!isnan(restored_best_freq_mhz) && restored_best_freq_mhz >= PCM_FREQ_MIN_MHZ &&
        restored_best_vcore_mv >= PCM_VCORE_MIN_MV && restored_best_vcore_mv <= PCM_VCORE_MAX_MV) {
        state->cur_freq_idx = dvfs_bandit_freq_to_idx(restored_best_freq_mhz);
        state->cur_vcore_idx = dvfs_bandit_vcore_to_idx(restored_best_vcore_mv);
        state->best_safe_freq_mhz = dvfs_bandit_idx_to_freq(state->cur_freq_idx);
        state->best_safe_vcore_mv = dvfs_bandit_idx_to_vcore(state->cur_vcore_idx);
    } else {
        state->cur_freq_idx = dvfs_bandit_freq_to_idx(state->baseline_freq_mhz);
        state->cur_vcore_idx = dvfs_bandit_vcore_to_idx(state->baseline_vcore_mv);
        state->best_safe_freq_mhz = 0.0f;
        state->best_safe_vcore_mv = 0;
    }
}

void dvfs_bandit_apply_decay(DvfsBanditState * state, float gamma)
{
    if (state == NULL) {
        return;
    }

    float g = isnan(gamma) ? 1.0f : fmaxf(0.0f, fminf(1.0f, gamma));
    for (int f = 0; f < DVFS_BANDIT_FREQ_STEPS; f++) {
        for (int v = 0; v < DVFS_BANDIT_VCORE_STEPS; v++) {
            state->cells[f][v].weight_n *= g;
            state->cells[f][v].reward_sum *= g;
        }
    }
}

void dvfs_bandit_step(DvfsBanditState * state,
                      const DvfsBanditSample * sample,
                      float * out_freq_mhz,
                      uint16_t * out_vcore_mv,
                      float * out_best_freq_mhz,
                      uint16_t * out_best_vcore_mv)
{
    if (state == NULL) {
        return;
    }

    float cur_freq = dvfs_bandit_idx_to_freq(state->cur_freq_idx);
    uint16_t cur_vcore = dvfs_bandit_idx_to_vcore(state->cur_vcore_idx);

    if (sample == NULL) {
        if (out_freq_mhz != NULL) *out_freq_mhz = cur_freq;
        if (out_vcore_mv != NULL) *out_vcore_mv = cur_vcore;
        if (out_best_freq_mhz != NULL) *out_best_freq_mhz = state->best_safe_freq_mhz;
        if (out_best_vcore_mv != NULL) *out_best_vcore_mv = state->best_safe_vcore_mv;
        return;
    }

    state->total_steps++;
    state->dwell_steps++;

    dvfs_bandit_apply_decay(state, DVFS_BANDIT_DISCOUNT_GAMMA);

    float chip_temp = (!isnan(sample->chip_temp_c) && sample->chip_temp_c > 0.0f) ? sample->chip_temp_c : 0.0f;
    float vr_temp = (!isnan(sample->vr_temp_c) && sample->vr_temp_c >= 0.0f) ? sample->vr_temp_c : 0.0f;
    float err_pct = (!isnan(sample->error_percentage) && sample->error_percentage >= 0.0f) ? sample->error_percentage : 0.0f;
    float power_w = (!isnan(sample->power_w) && sample->power_w >= 0.0f) ? sample->power_w : 0.0f;

    if (!state->ewma_initialized) {
        state->ewma_chip_temp_c = chip_temp;
        state->ewma_err_pct = err_pct;
        state->ewma_initialized = true;
    } else {
        if ((chip_temp > 0.0f && state->ewma_chip_temp_c > 0.0f &&
             fabsf(chip_temp - state->ewma_chip_temp_c) >= DVFS_BANDIT_DRIFT_TEMP_THRESH_C) ||
            fabsf(err_pct - state->ewma_err_pct) >= DVFS_BANDIT_DRIFT_ERR_THRESH_PCT) {
            dvfs_bandit_apply_decay(state, DVFS_BANDIT_DRIFT_DECAY_GAMMA);
        }
        state->ewma_chip_temp_c = (0.25f * chip_temp) + (0.75f * state->ewma_chip_temp_c);
        state->ewma_err_pct = (0.25f * err_pct) + (0.75f * state->ewma_err_pct);
    }

    DvfsBanditCell * cur_cell = &state->cells[state->cur_freq_idx][state->cur_vcore_idx];
    float heff = dvfs_bandit_compute_effective_hashrate(sample->hashrate_ghs, err_pct);
    float reward = dvfs_bandit_compute_reward(sample, false);

    cur_cell->weight_n += 1.0f;
    cur_cell->reward_sum += reward;
    cur_cell->avg_heff_ghs = (cur_cell->avg_heff_ghs <= 0.0f) ? heff : (0.5f * cur_cell->avg_heff_ghs + 0.5f * heff);
    cur_cell->last_visited_step = state->total_steps;

    bool clean_sample = (err_pct <= 0.8f) && (power_w <= 30.5f) && (chip_temp < 65.5f) &&
                        (vr_temp < 84.0f) && (heff > 0.0f);
    if (clean_sample) {
        cur_cell->clean_streak++;
        if (cur_cell->clean_streak >= DVFS_BANDIT_SAFE_STABLE_STEPS) {
            if (state->best_safe_vcore_mv == 0 || cur_cell->avg_heff_ghs >= state->best_safe_heff_ghs) {
                state->best_safe_freq_mhz = cur_freq;
                state->best_safe_vcore_mv = cur_vcore;
                state->best_safe_heff_ghs = cur_cell->avg_heff_ghs;
            }
        }
    } else {
        cur_cell->clean_streak = 0;
    }

    bool need_pullback = (power_w > 30.8f) || (chip_temp >= 66.5f) || (vr_temp >= 86.0f) || (err_pct > 1.0f);
    if (need_pullback) {
        if (err_pct > 1.0f && power_w < 30.0f && vr_temp < 84.0f &&
            state->cur_vcore_idx < (DVFS_BANDIT_VCORE_STEPS - 1)) {
            state->cur_vcore_idx++;
            state->dwell_steps = 0;
        } else if (state->cur_freq_idx > 0) {
            state->cur_freq_idx--;
            state->dwell_steps = 0;
        }
    } else if (state->recovery_cooldown_steps > 0) {
        state->recovery_cooldown_steps--;
    } else if (state->dwell_steps >= DVFS_BANDIT_MIN_DWELL_STEPS) {
        float total_weight = 0.0f;
        for (int f = 0; f < DVFS_BANDIT_FREQ_STEPS; f++) {
            for (int v = 0; v < DVFS_BANDIT_VCORE_STEPS; v++) {
                total_weight += state->cells[f][v].weight_n;
            }
        }
        if (total_weight < 1.0f) {
            total_weight = 1.0f;
        }

        bool allow_positive = (power_w < 30.0f) && (chip_temp < 64.5f) && (vr_temp < 82.0f);
        float cur_mean = (cur_cell->weight_n > 0.01f) ? (cur_cell->reward_sum / cur_cell->weight_n) : reward;
        float cur_ucb = cur_mean + (DVFS_BANDIT_UCB_C * sqrtf(logf(total_weight + 1.0f) / fmaxf(0.25f, cur_cell->weight_n)));

        uint8_t best_f = state->cur_freq_idx;
        uint8_t best_v = state->cur_vcore_idx;
        float best_ucb = cur_ucb;

        for (int df = -1; df <= 1; df++) {
            for (int dv = -1; dv <= 1; dv++) {
                if (df == 0 && dv == 0) {
                    continue;
                }
                if (df == 1 && dv == -1) {
                    continue;
                }
                if (!allow_positive && (df > 0 || dv > 0)) {
                    continue;
                }

                int32_t nf = (int32_t) state->cur_freq_idx + df;
                int32_t nv = (int32_t) state->cur_vcore_idx + dv;
                if (nf < 0 || nf >= DVFS_BANDIT_FREQ_STEPS || nv < 0 || nv >= DVFS_BANDIT_VCORE_STEPS) {
                    continue;
                }

                const DvfsBanditCell * cand = &state->cells[nf][nv];
                if (cand->blacklist_until_step > state->total_steps) {
                    continue;
                }

                float ucb;
                if (cand->weight_n < 0.05f) {
                    ucb = cur_mean + (DVFS_BANDIT_UCB_C * sqrtf(logf(total_weight + 1.0f) / 0.25f));
                } else {
                    float mean = cand->reward_sum / cand->weight_n;
                    ucb = mean + (DVFS_BANDIT_UCB_C * sqrtf(logf(total_weight + 1.0f) / cand->weight_n));
                }

                if (ucb > (best_ucb + 0.01f)) {
                    best_ucb = ucb;
                    best_f = (uint8_t) nf;
                    best_v = (uint8_t) nv;
                }
            }
        }

        if (best_f != state->cur_freq_idx || best_v != state->cur_vcore_idx) {
            state->cur_freq_idx = best_f;
            state->cur_vcore_idx = best_v;
            state->dwell_steps = 0;
        }
    }

    if (out_freq_mhz != NULL) {
        *out_freq_mhz = dvfs_bandit_idx_to_freq(state->cur_freq_idx);
    }
    if (out_vcore_mv != NULL) {
        *out_vcore_mv = dvfs_bandit_idx_to_vcore(state->cur_vcore_idx);
    }
    if (out_best_freq_mhz != NULL) {
        *out_best_freq_mhz = state->best_safe_freq_mhz;
    }
    if (out_best_vcore_mv != NULL) {
        *out_best_vcore_mv = state->best_safe_vcore_mv;
    }
}

void dvfs_bandit_record_fault_and_rollback(DvfsBanditState * state,
                                           bool is_hard_watchdog,
                                           float * out_freq_mhz,
                                           uint16_t * out_vcore_mv)
{
    if (state == NULL) {
        return;
    }

    DvfsBanditCell * cur_cell = &state->cells[state->cur_freq_idx][state->cur_vcore_idx];
    cur_cell->weight_n += 1.0f;
    cur_cell->reward_sum -= DVFS_BANDIT_FAULT_PENALTY * (is_hard_watchdog ? 2.0f : 1.0f);
    cur_cell->fault_count++;
    cur_cell->clean_streak = 0;
    cur_cell->blacklist_until_step = state->total_steps +
                                     (is_hard_watchdog ? (DVFS_BANDIT_BLACKLIST_STEPS * 2U) : DVFS_BANDIT_BLACKLIST_STEPS);

    bool has_distinct_safe_point = false;
    if (state->best_safe_vcore_mv >= PCM_VCORE_MIN_MV && state->best_safe_freq_mhz >= PCM_FREQ_MIN_MHZ) {
        uint8_t safe_f = dvfs_bandit_freq_to_idx(state->best_safe_freq_mhz);
        uint8_t safe_v = dvfs_bandit_vcore_to_idx(state->best_safe_vcore_mv);
        if (safe_f != state->cur_freq_idx || safe_v != state->cur_vcore_idx) {
            state->cur_freq_idx = safe_f;
            state->cur_vcore_idx = safe_v;
            has_distinct_safe_point = true;
        }
    }

    if (!has_distinct_safe_point) {
        if (state->cur_freq_idx >= 2) {
            state->cur_freq_idx -= 2;
        } else if (state->cur_freq_idx > 0) {
            state->cur_freq_idx = 0;
        } else if (state->cur_vcore_idx < (DVFS_BANDIT_VCORE_STEPS - 1)) {
            state->cur_vcore_idx++;
        }

        state->best_safe_freq_mhz = 0.0f;
        state->best_safe_vcore_mv = 0;
        state->best_safe_heff_ghs = 0.0f;
    }

    state->dwell_steps = 0;
    state->recovery_cooldown_steps = DVFS_BANDIT_RECOVERY_COOLDOWN_STEPS;

    if (out_freq_mhz != NULL) {
        *out_freq_mhz = dvfs_bandit_idx_to_freq(state->cur_freq_idx);
    }
    if (out_vcore_mv != NULL) {
        *out_vcore_mv = dvfs_bandit_idx_to_vcore(state->cur_vcore_idx);
    }
}
