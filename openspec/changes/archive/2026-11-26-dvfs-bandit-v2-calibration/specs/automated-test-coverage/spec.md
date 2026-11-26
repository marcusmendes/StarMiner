## MODIFIED Requirements

### Requirement: Adaptive 2D Bandit Unit Tests in C
The `components/asic/test/` Unity suite SHALL test the v2 `dvfs_bandit` controller and `PcmRtcDiagnostics` CRC32 round-trip, covering grid indexing, hard caps, continuous power penalty, physics-prior upward convergence without downward bias, `expected_hashrate` stall fallback, hysteresis, visited-cell immunity to error ripples, temporal decay, and pre-watchdog/watchdog rollback.

#### Scenario: Bandit grid convergence, penalties, and rollback pass Unity tests
- **WHEN** `test` / `test-ci` executes the `dvfs_bandit` and `power_control` Unity test cases covering fault rollback, `~0.6–1.0%` error ripples, `>= 2.5 °C` thermal drift, and `PcmRtcDiagnostics` serialization
- **THEN** assertions verify that step commands respect grid step sizes and hard caps, pre-watchdog/watchdog feedback rolls back to the last proven safe point and penalizes the fault cell, visited cells retain their visited state across minor error ripples while real drift decays weights, and `PcmRtcDiagnostics` preserves `adaptive_best_freq_mhz` and `adaptive_best_vcore_mv` across CRC validation

#### Scenario: Bandit v2 converges upward without downward bias and penalizes VCore inflation
- **WHEN** `test` / `test-ci` exercises unvisited neighbor selection from a mid-range operating point (`600.0–625.0 MHz`) and fixed-frequency VCore sweeps below `28.0 W`
- **THEN** assertions verify the controller climbs when thermal/power/error headroom allows (rather than drifting to `500 MHz`), prefers lower wattage at equal frequency and clean error, and falls back to measured hashrate when actual output stalls well below `expected_hashrate`
