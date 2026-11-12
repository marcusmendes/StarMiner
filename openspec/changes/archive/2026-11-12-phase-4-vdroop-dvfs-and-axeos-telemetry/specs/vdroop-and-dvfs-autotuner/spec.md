# Spec Delta

## Purpose

Garante compensação automática de queda resistiva (*Vdroop*) em malha lenta (`10 s`, até `+20 mV`) e ajuste dinâmico de frequência e tensão em RAM segundo os 4 perfis DVFS (`Manual`, `Eficiência`, `Equilibrado` e `Performance`), respeitando os *Hard Caps* (`1000–1250 mV`, `400–675 MHz`, `32.0 W`).

## ADDED Requirements

### Requirement: Slow-Loop Vdroop Compensation
The system SHALL evaluate measured VCore (`core_voltage`) every `10 s` during normal stable mining (outside thermal soft-throttling and overheat mode, and after frequency transition stabilization) and adjust `vdroop_offset_mv` in `5 mV` steps up to a maximum cap of `+20 mV` while respecting the `1250 mV` VCore and `32.0 W` power limits.

#### Scenario: Resistive voltage drop increases Vdroop compensation
- **WHEN** `ASIC_initalized` is `true`, `!throttle_active`, `!overheat_mode`, `power <= 32.0 W`, at least `10 s` have elapsed since the last frequency change and last Vdroop evaluation, and `core_voltage < (base_voltage_mv - 5.0f)`
- **THEN** the system increases `vdroop_offset_mv` by `5 mV` up to a maximum of `20 mV` (clamped so `base_voltage_mv + vdroop_offset_mv <= 1250 mV`)

#### Scenario: Excess measured voltage or thermal throttling reduces or resets Vdroop offset
- **WHEN** `core_voltage > (base_voltage_mv + 5.0f)` during the `10 s` evaluation window or when `mining_stop()` / `overheat_mode` occurs
- **THEN** the system decreases `vdroop_offset_mv` by `5 mV` (down to `0 mV`) on overshoot, or resets `vdroop_offset_mv = 0` when mining stops

### Requirement: Manual and Efficiency DVFS Profiles
The system SHALL support `dvfs_profile = 0` (`Manual`) using the user's NVS target frequency and voltage, and `dvfs_profile = 1` (`Eficiência`) operating within `500–600 MHz` and stepping RAM VCore in `5 mV` / `10 mV` increments every `60 s` to maintain hash error rate `<= 0.5%`.

#### Scenario: Manual profile preserves user NVS target without auto-tuning offsets
- **WHEN** `dvfs_profile` is `0` (`Manual`)
- **THEN** the system zeroes DVFS auto-tuning offsets and uses `target_frequency` and `target_voltage` from NVS (combined with thermal, watchdog, and Vdroop offsets)

#### Scenario: Efficiency profile lowers VCore when error rate is within 0.5%
- **WHEN** `dvfs_profile` is `1` (`Eficiência`), `!throttle_active`, `!overheat_mode`, and `60 s` have elapsed with `error_percentage <= 0.5%`
- **THEN** the system clamps the base frequency within `500.0–600.0 MHz` and reduces the DVFS RAM voltage by `5 mV` (down to `1000 mV`); if `error_percentage > 0.5%`, it increases the DVFS RAM voltage by `10 mV` (up to `1250 mV`)

### Requirement: Balanced and Performance DVFS Profiles
The system SHALL support `dvfs_profile = 2` (`Equilibrado`, `525–625 MHz`, error `<= 0.8%`) and `dvfs_profile = 3` (`Performance`, `550–675 MHz`, error `<= 1.0%`), stepping RAM frequency and VCore every `60 s` within thermal, power (`32.0 W`), and voltage (`1000–1250 mV`) envelopes without modifying NVS target keys.

#### Scenario: Balanced profile scales frequency while cool and low-error
- **WHEN** `dvfs_profile` is `2` (`Equilibrado`), `!throttle_active`, `!overheat_mode`, and `60 s` have elapsed
- **THEN** the system operates within `525.0–625.0 MHz`, stepping RAM frequency up by `+12.5 MHz` (up to `625.0 MHz`) when `chip_temp_avg < 64.0 °C`, `vr_temp < 80.0 °C`, `power <= 32.0 W`, and `error_percentage <= 0.8%`, or stepping down by `-12.5 MHz` (down to `525.0 MHz`) when `error_percentage > 0.8%`

#### Scenario: Performance profile scales frequency and VCore up to envelope limits
- **WHEN** `dvfs_profile` is `3` (`Performance`), `!throttle_active`, `!overheat_mode`, and `60 s` have elapsed
- **THEN** the system operates within `550.0–675.0 MHz`, stepping RAM frequency up by `+12.5 MHz` (up to `675.0 MHz`) while `chip_temp_avg < 68.0 °C`, `vr_temp < 90.0 °C`, `power < 31.0 W`, and `error_percentage <= 1.0%`, or stepping RAM VCore up by `+10 mV` (up to `1250 mV`) / frequency down by `-12.5 MHz` when `error_percentage > 1.0%`
