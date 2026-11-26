# vdroop-and-dvfs-autotuner Specification

## Purpose

Garante compensação automática de queda resistiva (*Vdroop*) em malha lenta (`10 s`, até `+20 mV`) e ajuste dinâmico de frequência e tensão em RAM segundo os 4 perfis DVFS (`Manual`, `Eficiência`, `Equilibrado` e `Performance`), respeitando os *Hard Caps* (`1000–1250 mV`, `400–675 MHz`, `32.0 W`).

## Requirements

### Requirement: Slow-Loop Vdroop Compensation
The system SHALL evaluate measured VCore (`core_voltage`) every `10 s` during normal stable mining (outside thermal soft-throttling and overheat mode, and after frequency transition stabilization) and adjust `vdroop_offset_mv` in `5 mV` steps up to a maximum cap of `+20 mV` while respecting the `1250 mV` VCore and `32.0 W` power limits.

#### Scenario: Resistive voltage drop increases Vdroop compensation
- **WHEN** `ASIC_initalized` is `true`, `!throttle_active`, `!overheat_mode`, `power <= 32.0 W`, at least `10 s` have elapsed since the last frequency change and last Vdroop evaluation, and `core_voltage < (base_voltage_mv - 5.0f)`
- **THEN** the system increases `vdroop_offset_mv` by `5 mV` up to a maximum of `20 mV` (clamped so `base_voltage_mv + vdroop_offset_mv <= 1250 mV`)

#### Scenario: Excess measured voltage or thermal throttling reduces or resets Vdroop offset
- **WHEN** `core_voltage > (base_voltage_mv + 5.0f)` during the `10 s` evaluation window or when `mining_stop()` / `overheat_mode` occurs
- **THEN** the system decreases `vdroop_offset_mv` by `5 mV` (down to `0 mV`) on overshoot, or resets `vdroop_offset_mv = 0` when mining stops

### Requirement: Manual and Efficiency DVFS Profiles
The system SHALL support `dvfs_profile = 0` (`Manual`) using NVS targets, and `dvfs_profile = 1` (`Efficiency`) operating within `500–600 MHz`, stepping RAM VCore in `5 mV` / `10 mV` increments every `60 s` to keep hash error `<= 0.5%` and locking a learned minimum VCore floor upon errors or watchdog events.

#### Scenario: Manual profile preserves user NVS target without auto-tuning offsets
- **WHEN** `dvfs_profile` is `0` (`Manual`)
- **THEN** the system zeroes DVFS auto-tuning offsets and learned bounds, using `target_frequency` and `target_voltage` from NVS (combined with thermal, watchdog, and Vdroop offsets)

#### Scenario: Efficiency profile lowers VCore when error rate is within 0.5%
- **WHEN** `dvfs_profile` is `1` (`Efficiency`), `!throttle_active`, `!overheat_mode`, and `60 s` have elapsed with `error_percentage <= 0.5%`
- **THEN** the system clamps base frequency within `500.0–600.0 MHz` and reduces DVFS RAM VCore by `5 mV` only while above `max(1000 mV, learned_min_vcore_mv)`; if `error_percentage > 0.5%` (with `power <= 32.0 W`), it increases VCore by `+10 mV` (up to `1250 mV`) and locks `learned_min_vcore_mv` at that stepped-up voltage

#### Scenario: Changing NVS target resets learned DVFS bounds
- **WHEN** the user modifies `target_frequency`, `target_voltage`, or `dvfs_profile` in NVS
- **THEN** the system clears `learned_min_vcore_mv` and `learned_max_freq_mhz` so the chip re-evaluates stability under the new configuration

### Requirement: Balanced and Performance DVFS Profiles
The system SHALL support `dvfs_profile = 2` (`Balanced`, `525–625 MHz`, fixed NVS VCore, error `<= 0.8%`) and `dvfs_profile = 3` (`Performance`, `550–675 MHz`, error `<= 1.0%`), locking per-chip learned limits, applying preventive thermal/power pullback before soft-throttling, and resetting learned limits on NVS changes.

#### Scenario: Balanced profile scales frequency while cool and low-error
- **WHEN** `dvfs_profile` is `2` (`Balanced`), `!throttle_active`, `!overheat_mode`, and `60 s` have elapsed
- **THEN** the system keeps `dvfs_voltage_offset_mv = 0`, scales RAM frequency up by `+12.5 MHz` up to `min(625.0 MHz, learned_max_freq_mhz)` while `chip_temp_avg < 64.0 °C`, `vr_temp < 80.0 °C`, `power <= 32.0 W`, and `error_percentage <= 0.8%`, or steps down by `-12.5 MHz` (down to `525.0 MHz`) and locks `learned_max_freq_mhz` when `error_percentage > 0.8%`

#### Scenario: Balanced profile performs preventive thermal pullback without locking ceiling
- **WHEN** `dvfs_profile` is `2` (`Balanced`), `chip_temp_avg >= 65.5 °C` or `vr_temp >= 84.0 °C` (below soft-throttling thresholds), `error_percentage <= 0.8%`, and current DVFS frequency exceeds `target_frequency`
- **THEN** the system steps DVFS frequency down by `-12.5 MHz` toward `target_frequency` without locking `learned_max_freq_mhz`, allowing recovery up to `learned_max_freq_mhz` once temperatures drop back below `64.0 °C` ASIC and `80.0 °C` VR

#### Scenario: Performance profile scales frequency and VCore up to envelope limits
- **WHEN** `dvfs_profile` is `3` (`Performance`), `!throttle_active`, `!overheat_mode`, and `60 s` have elapsed
- **THEN** the system steps frequency up by `+12.5 MHz` up to `min(675.0 MHz, learned_max_freq_mhz)` while cool and `power < 31.0 W`; on `error_percentage > 1.0%`, it steps VCore up by `+10 mV` and locks `learned_min_vcore_mv` if below `1250 mV` and `< 31.0 W`, or steps frequency down by `-12.5 MHz` and locks `learned_max_freq_mhz` otherwise; and if `chip_temp_avg >= 66.5 °C`, `vr_temp >= 87.0 °C`, or `power >= 31.2 W` while above `target_frequency`, it pulls back `-12.5 MHz` without locking `learned_max_freq_mhz`

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
