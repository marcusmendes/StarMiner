# Spec Delta

## ADDED Requirements

### Requirement: Target Frequency and Voltage Preservation in AxeOS Edit Form
The AxeOS Settings form (`EditComponent`) SHALL initialize its `frequency` and `coreVoltage` controls from `targetFrequency` and `targetVoltage` (falling back to `frequency` and `coreVoltage` only when target fields are absent), and SHALL provide a `dvfsProfile` selector (`0..3`) whose changes save via `PATCH /api/system` without requiring a device restart.

#### Scenario: Opening Settings during RAM soft-throttling or Vdroop preserves NVS targets
- **WHEN** `GET /api/system/info` returns `frequency = 475`, `targetFrequency = 550`, `coreVoltage = 1165`, and `targetVoltage = 1150`
- **THEN** the `EditComponent` form initializes `frequency` to `550` and `coreVoltage` to `1150` so saving unrelated settings (such as fan or display) does not overwrite the NVS targets with transient RAM values

#### Scenario: Changing dvfsProfile without triggering a restart prompt
- **WHEN** the user selects a new `dvfsProfile` value (`0 = Manual`, `1 = Efficiency`, `2 = Balanced`, or `3 = Performance`) in `EditComponent` and clicks Save
- **THEN** `updateSystem()` sends `{ dvfsProfile }` in `PATCH /api/system` and `isRestartRequired` evaluates to `false`

### Requirement: Thermal Throttling Banner, Vdroop Offset, and Resilience Counters in AxeOS UI
The AxeOS `HomeComponent` and `SystemComponent` SHALL render `throttleActive`, `targetFrequency`, `vdroopOffsetMv`, `dvfsProfile`, `watchdogResets`, and `cmlErrorCount` telemetry for the user and SHALL evaluate low-frequency warnings against `targetFrequency`.

#### Scenario: Soft-throttling warning banner appears on Home when throttleActive is 1
- **WHEN** `LiveDataService.info$` emits `throttleActive = 1`
- **THEN** `HomeComponent.handleSystemMessages()` displays a `warn` message with type `THERMAL_THROTTLE` and removes it once `throttleActive` returns to `0`

#### Scenario: Transient RAM frequency drop does not trigger FREQUENCY_LOW warning
- **WHEN** `info.frequency < 400` while `info.targetFrequency >= 400`
- **THEN** `HomeComponent.handleSystemMessages()` does not emit the `FREQUENCY_LOW` warning

#### Scenario: Power card and System table expose DVFS profile, Vdroop offset, and fault counters
- **WHEN** `info` contains `dvfsProfile = 2`, `vdroopOffsetMv = 15`, `watchdogResets = 1`, and `cmlErrorCount = 3`
- **THEN** the Home Power card shows the active DVFS profile and `+15 mV` Vdroop compensation indicator, and `SystemComponent.getSystemRows()` includes rows for `DVFS Profile`, `ASIC Watchdog Resets` (`1`), and `PMBus CML Recoveries` (`3`)
