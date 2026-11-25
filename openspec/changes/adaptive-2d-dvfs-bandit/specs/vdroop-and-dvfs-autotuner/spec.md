## ADDED Requirements

### Requirement: Adaptive 2D Bandit DVFS Profile
The system SHALL support opt-in `dvfs_profile = 4` (`Adaptive`), optimizing effective hashrate over a discrete `(frequency, VCore)` grid in `12.5 MHz` and `10 mV` steps within `400.0–675.0 MHz`, `1000–1250 mV`, and `<= 32.0 W` using Discounted UCB with ASIC error, power, and thermal penalties, minimum dwell hysteresis, and periodic discount decay.

#### Scenario: Adaptive profile explores and converges within hard caps
- **WHEN** `dvfs_profile` is `4` (`Adaptive`), `ASIC_initalized` is `true`, `!throttle_active`, `!overheat_mode`, and the evaluation interval (`>= 60 s`) elapses
- **THEN** the controller updates reward statistics for the current `(frequency, VCore)` cell from $H_{\text{eff}}$ minus error, power, and temperature penalties, selects the highest Discounted UCB neighbor (`±1` step on grid within hard caps), and applies new RAM targets only after minimum dwell hysteresis

#### Scenario: Temporal discounting and drift detection trigger re-exploration
- **WHEN** temporal discount factor decay applies across evaluation windows or rolling temperature/error drift exceeds the configured threshold
- **THEN** older cell weights decay geometrically so the controller re-evaluates neighboring `(frequency, VCore)` points under the current ambient and silicon conditions

### Requirement: Adaptive DVFS Rollback, Pre-Watchdog Feedback, and RTC Persistence
Upon `pre_watchdog_trigger` or ASIC watchdog reset while in `Adaptive` (`4`), the system SHALL immediately step down to the last proven safe `(frequency, VCore)` point, heavily penalize the triggering grid cell, and preserve `adaptive_best_freq_mhz` and `adaptive_best_vcore_mv` across warm resets in `PcmRtcDiagnostics`.

#### Scenario: Pre-watchdog or watchdog event triggers immediate rollback and penalty
- **WHEN** `dvfs_profile` is `4` (`Adaptive`) and a `pre_watchdog_trigger` event or `ASIC_watchdog` reset occurs at the active grid cell
- **THEN** the controller records a severe negative reward and temporary exclusion for that cell, immediately restores RAM targets to the last proven safe point (or NVS safe baseline if none exists), and suppresses positive steps during the recovery cooldown

#### Scenario: Warm reset restores adaptive safe point unless NVS configuration changes
- **WHEN** the device performs a warm reset (`esp_reset_reason() != ESP_RST_POWERON`) with matching NVS fingerprint and valid `PcmRtcDiagnostics` magic and CRC32
- **THEN** the system restores `adaptive_best_freq_mhz` and `adaptive_best_vcore_mv` and initializes the adaptive controller near that safe point; if NVS `target_frequency`, `target_voltage`, or `dvfs_profile` changes, all bandit state and learned safe points are cleared
