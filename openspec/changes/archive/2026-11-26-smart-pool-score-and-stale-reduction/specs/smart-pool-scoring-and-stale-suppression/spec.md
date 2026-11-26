## Purpose

Define o cálculo de pontuação de saúde da pool normalizado pelo tempo de bloco da moeda, timeouts de socket Stratum adaptados à moeda, supressão local de shares obsoletos pós-`clean_jobs` e failover/failback inteligente opt-in com histerese.

## ADDED Requirements

### Requirement: Coin-Aware Rolling Pool Health Score
The system SHALL compute a bounded `0..100` rolling pool health score combining EWMA round-trip latency, rolling stale share rate normalized by `coin_block_time_s`, non-stale reject rate, and decaying disconnect penalties.

#### Scenario: Fast-block DGB tolerance vs slow-block BTC stale penalty
- **WHEN** evaluating `~140 ms` EWMA RTT and `~1.0%` stale rate under DGB (`15 s` block time) versus `2.5%` stale rate and high RTT under BTC (`600 s` block time)
- **THEN** the DGB pool maintains a high health score (`>= 80`), while the BTC pool score drops sharply to reflect excessive stale waste

#### Scenario: Disconnects and reject bursts penalize score with stable decay
- **WHEN** repeated disconnects or non-stale reject bursts occur, followed by sustained accepted low-latency shares
- **THEN** disconnect/reject penalties lower `pool_health_score` immediately and decay smoothly toward `100` during healthy mining

### Requirement: Generation-Based Local Stale Share Suppression
The system SHALL maintain an atomic `job_epoch` incremented on every `clean_jobs` (Stratum v1) or new `prev_hash` (Stratum v2), suppress network submission of expired non-block shares (`job_epoch != current_epoch` and `nonce_diff < network_nonce_diff`), increment `stale_shares_suppressed`, preserve `SYSTEM_notify_found_nonce` hashrate accounting, and still submit block candidates (`nonce_diff >= network_nonce_diff`).

#### Scenario: Normal obsolete share suppressed locally without corrupting hashrate accounting
- **WHEN** an ASIC share completes for a `job_id` from a superseded `job_epoch` with `nonce_diff < network_nonce_diff`
- **THEN** `asic_result_task` skips Stratum socket submission, increments `stale_shares_suppressed`, and still updates `SYSTEM_notify_found_nonce` and `scoreboard`

#### Scenario: Block-candidate share bypasses stale suppression
- **WHEN** a share from a superseded `job_epoch` meets or exceeds `network_nonce_diff`
- **THEN** `asic_result_task` submits the candidate share normally to give the miner every chance at a valid block

### Requirement: Coin-Aware Stratum Socket Keepalive and Receive Timeout
The system SHALL configure `SO_RCVTIMEO` and `TCP_KEEPIDLE`/`TCP_KEEPINTVL` using `coin_block_time_s` (`60 s` receive timeout and `20 s` keepalive idle for fast-block coins with `block_time_s <= 60 s`; `180 s` and `60 s` for slower chains like BTC) while keeping `TCP_NODELAY = 1`.

#### Scenario: Fast-block chain tightens receive timeout and keepalive
- **WHEN** connecting a Stratum socket while `coin_block_time_s <= 60` (e.g. `15 s` for DGB)
- **THEN** `stratum_socket_set_options_for_coin` enables `TCP_NODELAY`, sets `SO_RCVTIMEO = 60 s`, and sets `TCP_KEEPIDLE = 20 s`

#### Scenario: Slow-block chain preserves conservative socket timeouts
- **WHEN** connecting a Stratum socket for BTC (`coin_block_time_s = 600`)
- **THEN** `stratum_socket_set_options_for_coin` sets `SO_RCVTIMEO = 180 s` and `TCP_KEEPIDLE = 60 s`

### Requirement: Opt-In Hysteresis-Guarded Smart Pool Failover and Failback
When `stratumSmartFailover` is enabled and a fallback pool exists (without `use_fallback_stratum` override), the coordinator SHALL switch to fallback only after sustained low health (`< 45` across hysteresis checks), cooldown (`>= 600 s`), and a successful standby probe, and SHALL probe primary periodically to fail back only after cooldown.

#### Scenario: Sustained primary degradation triggers probe-verified failover
- **WHEN** `stratumSmartFailover` is `true`, primary health stays `< 45` across `>= 3` consecutive checks past minimum sample and `600 s` cooldown thresholds, and standby probe succeeds
- **THEN** `protocol_coordinator` switches to the fallback pool, resets active score accumulators, and increments `smartFailoverCount`

#### Scenario: Disabled smart failover, cooldown, or failed probe prevents flapping
- **WHEN** `stratumSmartFailover` is `false`, cooldown has not elapsed, or the alternate pool fails its TCP/handshake probe
- **THEN** `protocol_coordinator` stays on the current active pool without triggering a failover switch
