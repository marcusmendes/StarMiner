#include "unity.h"
#include "pool_score.h"

#include <stdbool.h>
#include <stdint.h>

static int64_t sec_to_us(int64_t sec)
{
    return sec * 1000000LL;
}

TEST_CASE("pool_score_init sets neutral healthy defaults", "[pool_score]")
{
    PoolScoreState state;
    pool_score_init(&state);

    TEST_ASSERT_FLOAT_WITHIN(0.01f, 100.0f, state.score);
    TEST_ASSERT_EQUAL_UINT32(0, state.sample_count);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, state.stale_rate_pct);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, state.rtt_ewma_ms);
    TEST_ASSERT_FALSE(pool_score_should_failover(&state, sec_to_us(100)));
}

TEST_CASE("pool_score keeps fast-block DGB healthy within natural stale budget", "[pool_score]")
{
    PoolScoreState state;
    pool_score_init(&state);

    // DGB 15s block time, ~140ms RTT -> budget = 100 * (0.25 + 0.14) / 15 = 2.60%
    // 1 stale out of 100 shares (1.0% stale) should have zero stale penalty.
    float score = 100.0f;
    for (int i = 0; i < 100; i++) {
        bool accepted = (i != 50);
        bool is_stale = (i == 50);
        score = pool_score_record_share(&state, accepted, is_stale, 140.0f, 15, sec_to_us(10 + i * 2));
    }

    TEST_ASSERT_FLOAT_WITHIN(0.1f, 1.0f, state.stale_rate_pct);
    TEST_ASSERT_TRUE(score >= 80.0f);
    TEST_ASSERT_TRUE(score <= 100.0f);
    TEST_ASSERT_FALSE(pool_score_should_failover(&state, sec_to_us(300)));
}

TEST_CASE("pool_score penalizes excess stale more sharply on 600s BTC than 15s DGB", "[pool_score]")
{
    PoolScoreState btc_state;
    PoolScoreState dgb_state;
    pool_score_init(&btc_state);
    pool_score_init(&dgb_state);

    // 40 shares with 1 stale (2.5% stale) at 140ms RTT:
    // DGB budget = 2.60% -> zero excess stale -> score ~97
    // BTC budget = 0.25% -> 2.25% excess stale -> 45pt stale penalty -> score < 60
    float btc_score = 100.0f;
    float dgb_score = 100.0f;
    for (int i = 0; i < 40; i++) {
        bool accepted = (i != 20);
        bool is_stale = (i == 20);
        btc_score = pool_score_record_share(&btc_state, accepted, is_stale, 140.0f, 600, sec_to_us(10 + i * 5));
        dgb_score = pool_score_record_share(&dgb_state, accepted, is_stale, 140.0f, 15, sec_to_us(10 + i * 5));
    }

    TEST_ASSERT_TRUE(dgb_score >= 90.0f);
    TEST_ASSERT_TRUE(btc_score < 60.0f);
    TEST_ASSERT_TRUE(dgb_score > btc_score);
}

TEST_CASE("pool_score bounds stay within 0..100 under extreme degradation", "[pool_score]")
{
    PoolScoreState state;
    pool_score_init(&state);

    float score = 100.0f;
    for (int i = 0; i < 50; i++) {
        score = pool_score_record_share(&state, false, true, 5000.0f, 600, sec_to_us(10 + i));
    }
    score = pool_score_record_disconnect(&state, 600, sec_to_us(70));
    score = pool_score_record_disconnect(&state, 600, sec_to_us(75));
    score = pool_score_record_disconnect(&state, 600, sec_to_us(80));

    TEST_ASSERT_TRUE(score >= 0.0f);
    TEST_ASSERT_TRUE(score <= 100.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 0.0f, score);
}

TEST_CASE("pool_score applies disconnect penalty and decays over time", "[pool_score]")
{
    PoolScoreState state;
    pool_score_init(&state);

    pool_score_record_share(&state, true, false, 80.0f, 600, sec_to_us(10));
    float after_dc1 = pool_score_record_disconnect(&state, 600, sec_to_us(20));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 75.0f, after_dc1);

    float after_dc2 = pool_score_record_disconnect(&state, 600, sec_to_us(25));
    TEST_ASSERT_TRUE(after_dc2 < 52.0f);

    // After 900 seconds without further disconnects, penalty decays completely (60 / 1/15 = 900s)
    float recovered = pool_score_get_current(&state, 600, sec_to_us(925));
    TEST_ASSERT_FLOAT_WITHIN(0.1f, 100.0f, recovered);
}

TEST_CASE("pool_score enforces sample, streak, and cooldown hysteresis for failover", "[pool_score]")
{
    PoolScoreState state;
    pool_score_init(&state);

    // First 9 degraded shares -> sample_count < 10 -> MUST NOT fail over
    for (int i = 0; i < 9; i++) {
        pool_score_record_share(&state, false, true, 800.0f, 600, sec_to_us(10 + i * 2));
    }
    TEST_ASSERT_TRUE(state.score < POOL_SCORE_FAILOVER_THRESHOLD);
    TEST_ASSERT_EQUAL_UINT32(9, state.sample_count);
    TEST_ASSERT_EQUAL_UINT32(0, state.degraded_streak);
    TEST_ASSERT_FALSE(pool_score_should_failover(&state, sec_to_us(30)));

    // 10th degraded share -> degraded_streak == 1 -> still needs streak >= 3
    pool_score_record_share(&state, false, true, 800.0f, 600, sec_to_us(32));
    TEST_ASSERT_EQUAL_UINT32(1, state.degraded_streak);
    TEST_ASSERT_FALSE(pool_score_should_failover(&state, sec_to_us(32)));

    // 11th degraded share -> degraded_streak == 2 -> still < 3
    pool_score_record_share(&state, false, true, 800.0f, 600, sec_to_us(34));
    TEST_ASSERT_EQUAL_UINT32(2, state.degraded_streak);
    TEST_ASSERT_FALSE(pool_score_should_failover(&state, sec_to_us(34)));

    // 12th degraded share -> degraded_streak == 3 -> triggers failover
    pool_score_record_share(&state, false, true, 800.0f, 600, sec_to_us(36));
    TEST_ASSERT_EQUAL_UINT32(3, state.degraded_streak);
    TEST_ASSERT_TRUE(pool_score_should_failover(&state, sec_to_us(36)));

    // Mark pool switch -> sets cooldown for 600s
    pool_score_reset_for_pool(&state, sec_to_us(40), true);
    TEST_ASSERT_TRUE(pool_score_in_cooldown(&state, sec_to_us(340)));

    // Degrade again inside cooldown window -> should NOT allow failover yet
    for (int i = 0; i < 15; i++) {
        pool_score_record_share(&state, false, true, 800.0f, 600, sec_to_us(100 + i * 2));
    }
    TEST_ASSERT_FALSE(pool_score_should_failover(&state, sec_to_us(340)));

    // Once 600s cooldown elapses (40 + 601 = 641s), failover is permitted again
    TEST_ASSERT_FALSE(pool_score_in_cooldown(&state, sec_to_us(641)));
    TEST_ASSERT_TRUE(pool_score_should_failover(&state, sec_to_us(641)));
}

TEST_CASE("pool_score_is_stale_error detects case-insensitive stale reasons", "[pool_score]")
{
    TEST_ASSERT_TRUE(pool_score_is_stale_error("Stale"));
    TEST_ASSERT_TRUE(pool_score_is_stale_error("stale share"));
    TEST_ASSERT_TRUE(pool_score_is_stale_error("Job not found"));
    TEST_ASSERT_TRUE(pool_score_is_stale_error("JOB-NOT-FOUND"));
    TEST_ASSERT_TRUE(pool_score_is_stale_error("Work expired"));

    TEST_ASSERT_FALSE(pool_score_is_stale_error("Low difficulty share"));
    TEST_ASSERT_FALSE(pool_score_is_stale_error("Duplicate"));
    TEST_ASSERT_FALSE(pool_score_is_stale_error(""));
    TEST_ASSERT_FALSE(pool_score_is_stale_error(NULL));
}

TEST_CASE("pool_score_should_suppress_stale protects block candidates across epochs", "[pool_score]")
{
    // Current epoch share is never suppressed
    TEST_ASSERT_FALSE(pool_score_should_suppress_stale(5, 5, 1000.0, 50000.0));

    // Ordinary share from old epoch is suppressed
    TEST_ASSERT_TRUE(pool_score_should_suppress_stale(4, 5, 1000.0, 50000.0));

    // Old epoch share with unknown network_diff is suppressed
    TEST_ASSERT_TRUE(pool_score_should_suppress_stale(4, 5, 100000.0, 0.0));

    // Block candidate (nonce_diff >= network_diff) is NEVER suppressed even across epochs
    TEST_ASSERT_FALSE(pool_score_should_suppress_stale(4, 5, 50000.0, 50000.0));
    TEST_ASSERT_FALSE(pool_score_should_suppress_stale(4, 5, 999999.0, 50000.0));
}

TEST_CASE("pool_score tolerates single natural stale during early DGB warmup", "[pool_score]")
{
    PoolScoreState state;
    pool_score_init(&state);

    float score = 100.0f;
    for (int i = 0; i < 11; i++) {
        score = pool_score_record_share(&state, true, false, 150.0f, 15, sec_to_us(10 + i * 10));
    }
    score = pool_score_record_share(&state, false, true, 150.0f, 15, sec_to_us(120));

    TEST_ASSERT_FLOAT_WITHIN(0.2f, 8.33f, state.stale_rate_pct);
    TEST_ASSERT_TRUE(score >= 80.0f);
    TEST_ASSERT_FALSE(pool_score_should_failover(&state, sec_to_us(120)));
}
