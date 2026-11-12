# ram-thermal-throttling-and-safe-overheat Specification

## Purpose

Garante proteção térmica progressiva em RAM (Soft-Throttling com histerese de recuperação) e desligamento crítico de emergência (`overheat_mode`) para o Bitaxe Gamma (`BM1370` + `TPS546D24A`), separando o estado efetivo em RAM das configurações alvo do usuário e impedindo qualquer sobrescrita destrutiva de tensão, frequência ou modo de ventilação na NVS.

## Requirements

### Requirement: Separation of NVS Target and Effective RAM State with Hard Caps
The system SHALL maintain user-configured voltage and frequency in NVS strictly as target values while tracking effective operating voltage and frequency in RAM, clamping applied hardware settings within safe operating limits (`1000–1250 mV` VCore, `400 MHz` minimum floor, and `32.0 W` input power).

#### Scenario: User target values read from NVS and clamped to safe hard caps
- **WHEN** `power_management_task` reads `NVS_CONFIG_ASIC_FREQUENCY` and `NVS_CONFIG_ASIC_VOLTAGE` on each control cycle
- **THEN** the system stores them as `target_frequency` and `target_voltage` in RAM and clamps any effective VCore sent to `VCORE_set_voltage()` to the range `1000 mV` to `1250 mV` and any effective frequency during active mining to a minimum floor of `400.0 MHz`

#### Scenario: Effective RAM frequency combines thermal and watchdog offsets
- **WHEN** thermal soft-throttling (`thermal_offset_mhz`) or watchdog backoff (`watchdog_freq_offset_mhz`) is active in RAM
- **THEN** the system computes the effective RAM frequency from `target_frequency` minus active RAM offsets (clamped at `>= 400.0 MHz`) and applies transitions via `ASIC_set_frequency()` without modifying `NVS_CONFIG_ASIC_FREQUENCY`

### Requirement: Progressive RAM Soft-Throttling
The system SHALL activate progressive RAM soft-throttling when ASIC temperature, VR temperature, or input power exceeds warning thresholds, forcing fan speed to `100%` in RAM without modifying NVS fan settings and stepping down RAM frequency and VCore every `10 s` down to the safe operating floor.

#### Scenario: Standard soft-throttling step on thermal or power warning
- **WHEN** `chip_temp_avg >= 68.0 °C`, `vr_temp >= 90.0 °C`, or `power > 32.0 W` while below severe warning thresholds (`chip_temp_avg < 72.0 °C` and `vr_temp < 98.0 °C`)
- **THEN** the system sets `throttle_active = true`, forces fan PWM to `100%` in RAM without writing `NVS_CONFIG_AUTO_FAN_SPEED` or `NVS_CONFIG_MANUAL_FAN_SPEED`, and every `10 s` reduces effective RAM frequency by `12.5 MHz` (down to `400.0 MHz`) and effective RAM VCore by `10 mV` (down to `1000 mV`)

#### Scenario: Accelerated soft-throttling step on severe thermal warning
- **WHEN** `chip_temp_avg >= 72.0 °C` or `vr_temp >= 98.0 °C` (and below critical emergency shutdown thresholds)
- **THEN** the system sets `throttle_active = true`, forces fan PWM to `100%` in RAM, and every `10 s` reduces effective RAM frequency by `25.0 MHz` (down to `400.0 MHz`) and effective RAM VCore by `10 mV` (down to `1000 mV`)

### Requirement: Hysteresis Thermal Recovery in RAM
The system SHALL automatically step effective RAM frequency and VCore back up toward the NVS target when temperatures and input power remain continuously within the safe recovery zone for `30 s`, clearing `throttle_active` and restoring normal fan control once target values are reached.

#### Scenario: Gradual step-up after 30 seconds in safe hysteresis zone
- **WHEN** `throttle_active` is `true` and `chip_temp_avg < 65.0 °C`, `vr_temp < 85.0 °C`, and `power <= 32.0 W` continuously for at least `30 s`
- **THEN** every `15 s` the system increases effective RAM frequency by `12.5 MHz` and effective RAM VCore by `10 mV` up to `target_frequency` and `target_voltage`

#### Scenario: Full recovery to target clears throttle state and restores fan mode
- **WHEN** effective RAM frequency and VCore reach `target_frequency` and `target_voltage` during hysteresis recovery
- **THEN** the system sets `throttle_active = false` and `fan_controller_task` resumes the user-configured fan mode (`NVS_CONFIG_AUTO_FAN_SPEED` PID or `NVS_CONFIG_MANUAL_FAN_SPEED`)

### Requirement: Safe Critical Overheat Shutdown without NVS Corruption
The system SHALL immediately stop mining, force fan speed to `100%` in RAM, and latch `overheat_mode` when ASIC or VR temperature reaches critical emergency thresholds, and SHALL NOT overwrite user-configured ASIC voltage, ASIC frequency, auto fan mode, or manual fan speed in NVS.

#### Scenario: Critical temperature triggers safe shutdown and preserves NVS settings
- **WHEN** `chip_temp_avg >= 75.0 °C` or `vr_temp >= 105.0 °C` (and sensor readings are valid)
- **THEN** the system invokes `mining_stop(GLOBAL_STATE)` (`VCore = 0 V`, `RST LOW`, stops job dispatch), sets `NVS_CONFIG_OVERHEAT_MODE = true` (`sys_module->overheat_mode = true`), keeps fan speed at `100%` in RAM, and leaves `NVS_CONFIG_ASIC_VOLTAGE`, `NVS_CONFIG_ASIC_FREQUENCY`, `NVS_CONFIG_AUTO_FAN_SPEED`, and `NVS_CONFIG_MANUAL_FAN_SPEED` completely untouched

#### Scenario: Clearing overheat mode resumes mining at original user target
- **WHEN** the user clears `overheat_mode` via AxeOS (`NVS_CONFIG_OVERHEAT_MODE = false`) after temperatures drop below safe limits
- **THEN** the system invokes `mining_start(GLOBAL_STATE)` and restores operation using the original user `target_frequency` and `target_voltage` stored in NVS
