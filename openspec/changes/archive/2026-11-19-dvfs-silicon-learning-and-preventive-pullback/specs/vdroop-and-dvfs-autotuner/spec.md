# Spec Delta

## MODIFIED Requirements

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
