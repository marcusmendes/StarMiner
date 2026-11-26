#include "pool_score.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

static float clampf_range(float val, float min_val, float max_val)
{
    if (val < min_val) {
        return min_val;
    }
    if (val > max_val) {
        return max_val;
    }
    return val;
}

static bool contains_icase(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL || needle[0] == '\0') {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p != '\0'; p++) {
        size_t i = 0;
        while (i < needle_len && p[i] != '\0' &&
               tolower((unsigned char)p[i]) == tolower((unsigned char)needle[i])) {
            i++;
        }
        if (i == needle_len) {
            return true;
        }
    }
    return false;
}

static void apply_disconnect_decay(PoolScoreState *state, int64_t now_us)
{
    if (state->last_decay_us > 0 && now_us > state->last_decay_us && state->disconnect_penalty > 0.0f) {
        float elapsed_s = (float)(now_us - state->last_decay_us) / 1000000.0f;
        state->disconnect_penalty -= elapsed_s / 15.0f;
        if (state->disconnect_penalty < 0.0f) {
            state->disconnect_penalty = 0.0f;
        }
    }
    if (now_us > 0) {
        state->last_decay_us = now_us;
    }
}

static float recompute_score(PoolScoreState *state, uint32_t block_time_s)
{
    uint32_t stale_sum = 0;
    for (uint8_t i = 0; i < state->window_count; i++) {
        stale_sum += state->stale_ring[i];
    }
    float scoring_stale_pct = (state->window_count > 0)
        ? (100.0f * (float)stale_sum / fmaxf((float)state->window_count, 40.0f))
        : 0.0f;

    float budget_pct = pool_score_compute_stale_budget_pct(state->rtt_ewma_ms, block_time_s);
    float excess_stale = fmaxf(0.0f, scoring_stale_pct - budget_pct);
    float stale_penalty = fminf(60.0f, excess_stale * 20.0f);
    float rtt_penalty = clampf_range((state->rtt_ewma_ms - 100.0f) / 15.0f, 0.0f, 35.0f);

    state->score = clampf_range(100.0f - stale_penalty - rtt_penalty - state->disconnect_penalty, 0.0f, 100.0f);
    return state->score;
}

void pool_score_init(PoolScoreState *state)
{
    if (state == NULL) {
        return;
    }
    memset(state, 0, sizeof(*state));
    state->score = 100.0f;
}

void pool_score_reset_for_pool(PoolScoreState *state, int64_t now_us, bool mark_switch)
{
    if (state == NULL) {
        return;
    }
    int64_t prev_switch_us = state->last_switch_us;
    memset(state, 0, sizeof(*state));
    state->score = 100.0f;
    state->last_decay_us = now_us;
    state->last_switch_us = mark_switch ? now_us : prev_switch_us;
}

float pool_score_compute_stale_budget_pct(float rtt_ms, uint32_t block_time_s)
{
    float rtt_s = fmaxf(0.0f, rtt_ms) / 1000.0f;
    float effective_block_time_s = fmaxf(5.0f, block_time_s > 0 ? (float)block_time_s : 600.0f);
    float raw = 100.0f * (0.25f + rtt_s) / effective_block_time_s;
    return clampf_range(raw, 0.25f, 3.0f);
}

bool pool_score_is_stale_error(const char *error_msg)
{
    if (error_msg == NULL || error_msg[0] == '\0') {
        return false;
    }
    return contains_icase(error_msg, "stale") ||
           contains_icase(error_msg, "job not found") ||
           contains_icase(error_msg, "job-not-found") ||
           contains_icase(error_msg, "expired");
}

float pool_score_record_share(PoolScoreState *state, bool accepted, bool is_stale,
                              float rtt_ms, uint32_t block_time_s, int64_t now_us)
{
    if (state == NULL) {
        return 100.0f;
    }

    apply_disconnect_decay(state, now_us);

    if (rtt_ms > 0.0f) {
        if (state->rtt_ewma_ms <= 0.0f) {
            state->rtt_ewma_ms = rtt_ms;
        } else {
            state->rtt_ewma_ms = 0.2f * rtt_ms + 0.8f * state->rtt_ewma_ms;
        }
    }

    state->stale_ring[state->ring_pos] = (!accepted && is_stale) ? 1 : 0;
    state->ring_pos = (uint8_t)((state->ring_pos + 1) % POOL_SCORE_WINDOW_SIZE);
    if (state->window_count < POOL_SCORE_WINDOW_SIZE) {
        state->window_count++;
    }
    state->sample_count++;

    uint32_t stale_sum = 0;
    for (uint8_t i = 0; i < state->window_count; i++) {
        stale_sum += state->stale_ring[i];
    }
    state->stale_rate_pct = state->window_count > 0
        ? (100.0f * (float)stale_sum / (float)state->window_count)
        : 0.0f;

    float score = recompute_score(state, block_time_s);

    if (state->sample_count >= POOL_SCORE_MIN_SAMPLES) {
        if (score < POOL_SCORE_FAILOVER_THRESHOLD) {
            state->degraded_streak++;
        } else {
            state->degraded_streak = 0;
        }
    }

    return score;
}

float pool_score_record_disconnect(PoolScoreState *state, uint32_t block_time_s, int64_t now_us)
{
    if (state == NULL) {
        return 100.0f;
    }

    apply_disconnect_decay(state, now_us);
    state->disconnect_penalty = fminf(60.0f, state->disconnect_penalty + 25.0f);

    float score = recompute_score(state, block_time_s);
    if (state->sample_count >= POOL_SCORE_MIN_SAMPLES) {
        if (score < POOL_SCORE_FAILOVER_THRESHOLD) {
            state->degraded_streak++;
        } else {
            state->degraded_streak = 0;
        }
    }
    return score;
}

float pool_score_get_current(PoolScoreState *state, uint32_t block_time_s, int64_t now_us)
{
    if (state == NULL) {
        return 100.0f;
    }
    apply_disconnect_decay(state, now_us);
    return recompute_score(state, block_time_s);
}

bool pool_score_should_failover(const PoolScoreState *state, int64_t now_us)
{
    if (state == NULL) {
        return false;
    }
    if (state->sample_count < POOL_SCORE_MIN_SAMPLES) {
        return false;
    }
    if (state->degraded_streak < POOL_SCORE_DEGRADED_STREAK) {
        return false;
    }
    if (state->score >= POOL_SCORE_FAILOVER_THRESHOLD) {
        return false;
    }
    if (state->last_switch_us > 0) {
        if (now_us < state->last_switch_us || (now_us - state->last_switch_us) < POOL_SCORE_COOLDOWN_US) {
            return false;
        }
    }
    return true;
}

bool pool_score_in_cooldown(const PoolScoreState *state, int64_t now_us)
{
    if (state == NULL || state->last_switch_us <= 0) {
        return false;
    }
    return now_us >= state->last_switch_us &&
           (now_us - state->last_switch_us) < POOL_SCORE_COOLDOWN_US;
}

bool pool_score_should_suppress_stale(uint32_t job_epoch, uint32_t current_epoch,
                                      double nonce_diff, double network_diff)
{
    if (job_epoch == current_epoch) {
        return false;
    }
    if (network_diff > 0.0 && nonce_diff >= network_diff) {
        return false;
    }
    return true;
}
