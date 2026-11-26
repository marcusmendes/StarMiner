#ifndef POOL_SCORE_H_
#define POOL_SCORE_H_

#include <stdbool.h>
#include <stdint.h>

#define POOL_SCORE_WINDOW_SIZE 100
#define POOL_SCORE_FAILOVER_THRESHOLD 45.0f
#define POOL_SCORE_MIN_SAMPLES 10
#define POOL_SCORE_DEGRADED_STREAK 3
#define POOL_SCORE_COOLDOWN_US (600LL * 1000000LL)

typedef struct {
    uint8_t stale_ring[POOL_SCORE_WINDOW_SIZE];
    uint8_t ring_pos;
    uint8_t window_count;
    uint32_t sample_count;
    float rtt_ewma_ms;
    float stale_rate_pct;
    float disconnect_penalty;
    int64_t last_decay_us;
    float score;
    uint32_t degraded_streak;
    int64_t last_switch_us;
} PoolScoreState;

void pool_score_init(PoolScoreState *state);

void pool_score_reset_for_pool(PoolScoreState *state, int64_t now_us, bool mark_switch);

float pool_score_compute_stale_budget_pct(float rtt_ms, uint32_t block_time_s);

bool pool_score_is_stale_error(const char *error_msg);

float pool_score_record_share(PoolScoreState *state, bool accepted, bool is_stale,
                              float rtt_ms, uint32_t block_time_s, int64_t now_us);

float pool_score_record_disconnect(PoolScoreState *state, uint32_t block_time_s, int64_t now_us);

float pool_score_get_current(PoolScoreState *state, uint32_t block_time_s, int64_t now_us);

bool pool_score_should_failover(const PoolScoreState *state, int64_t now_us);

bool pool_score_in_cooldown(const PoolScoreState *state, int64_t now_us);

bool pool_score_should_suppress_stale(uint32_t job_epoch, uint32_t current_epoch,
                                      double nonce_diff, double network_diff);

#endif /* POOL_SCORE_H_ */
