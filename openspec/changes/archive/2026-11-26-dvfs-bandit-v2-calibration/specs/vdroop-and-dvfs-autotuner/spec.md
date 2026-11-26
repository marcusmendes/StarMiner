## MODIFIED Requirements

### Requirement: Adaptive 2D Bandit DVFS Profile
The system SHALL support opt-in `dvfs_profile = 4` (`Adaptive`), optimizing low-variance effective hashrate (`expected_hashrate` safeguarded against stalls) minus ASIC error, continuous power efficiency, and thermal penalties over the `12.5 MHz` / `10 mV` grid (`400.0–675.0 MHz`, `1000–1250 mV`, `<= 32.0 W`), using unbiased physics UCB priors, dwell hysteresis, and drift decay that preserves visited-cell memory.

#### Scenario: Adaptive profile explores and converges within hard caps
- **WHEN** `dvfs_profile` is `4` (`Adaptive`), `ASIC_initalized` is `true`, `!throttle_active`, `!overheat_mode`, thermal/power headroom exists, and the evaluation interval (`>= 60 s`) elapses past minimum dwell hysteresis
- **THEN** the controller updates reward statistics from low-variance $H_{\text{eff}}$ minus error, continuous power efficiency, and temperature penalties, evaluates unvisited neighbors with an optimistic physics prior (no downward loop-order tie-breaking bias), and applies `±1` grid steps within hard caps

#### Scenario: Continuous power efficiency penalty prevents unnecessary VCore inflation at fixed frequency
- **WHEN** the chip operates below `28.0 W` at a fixed frequency with clean error rate (`error_percentage <= 0.3%`) and increasing `VCore` raises power draw without increasing effective hashrate
- **THEN** the continuous power penalty reduces reward monotonically with rising wattage and prevents unmotivated `+10 mV` steps

#### Scenario: Temporal discounting and drift detection trigger re-exploration
- **WHEN** minor transient exploration error ripples (`< 1.5%`) occur versus sustained thermal drift (`>= 2.5 °C`) or severe error jumps (`>= 1.5%`) across evaluation windows
- **THEN** previously visited grid cells retain their visited identity across gentle temporal discounting during normal operation, while real thermal/error drift accelerates forgetting so neighboring `(frequency, VCore)` points are re-evaluated
