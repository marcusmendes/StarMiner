# automated-test-coverage Specification

## Purpose

Garante cobertura de testes unitários automatizados no CI para a aritmética pura de regulação térmica, *Vdroop* e perfis DVFS no backend C e para os comportamentos críticos da interface Angular do `AxeOS`.

## Requirements

### Requirement: Pure Control Arithmetic Unit Tests in C
The project SHALL extract hardware-independent power, thermal offset, *Vdroop*, and DVFS boundary calculation functions into a testable C module inside `components/asic/` and verify them with Unity `TEST_CASE` assertions executed in `test` / `test-ci`.

#### Scenario: VCore clamping and Vdroop step bounds
- **WHEN** the Unity suite exercises VCore clamping and slow-loop *Vdroop* adjustment across boundary inputs (`< 1000 mV`, nominal `1150 mV`, `> 1250 mV`, measured voltage below `base - 5 mV`, and power above `32.0 W`)
- **THEN** effective VCore stays within `1000..1250 mV`, `vdroop_offset_mv` steps in `5 mV` increments up to `+20 mV`, and no positive step occurs when power exceeds `32.0 W`

#### Scenario: Effective frequency floor and DVFS profile envelopes
- **WHEN** the Unity suite evaluates effective frequency calculation with thermal/watchdog offsets and DVFS bounds for profiles `0..3`
- **THEN** effective frequency never drops below `400.0 MHz` and DVFS targets stay clamped within `[500, 600]`, `[525, 625]`, and `[550, 675] MHz` for profiles `1`, `2`, and `3`

### Requirement: AxeOS EditComponent Unit Tests for Target Preservation and DVFS Profile
The frontend unit test suite (`edit.component.spec.ts`) SHALL verify that `EditComponent` initializes from `targetFrequency` / `targetVoltage` and allows updating `dvfsProfile` without requiring a restart.

#### Scenario: EditComponent initializes form with NVS targets during RAM throttling
- **WHEN** `EditComponent` loads `SystemInfo` with `frequency = 475`, `targetFrequency = 550`, `coreVoltage = 1165`, `targetVoltage = 1150`, and `dvfsProfile = 2`
- **THEN** `form.value.frequency` equals `550`, `form.value.coreVoltage` equals `1150`, and `form.value.dvfsProfile` equals `2`

#### Scenario: Updating dvfsProfile keeps isRestartRequired false
- **WHEN** the user marks only `dvfsProfile` dirty in `EditComponent` and saves
- **THEN** `component.isRestartRequired` is `false` and `SystemApiService.updateSystem` receives `{ dvfsProfile }`

### Requirement: AxeOS HomeComponent and SystemComponent Telemetry Unit Tests
The frontend unit test suites (`home.component.spec.ts` and `system.component.spec.ts`) SHALL verify `THERMAL_THROTTLE` banner handling, `FREQUENCY_LOW` evaluation against `targetFrequency`, and resilience rows in `SystemComponent`.

#### Scenario: HomeComponent toggles THERMAL_THROTTLE banner and avoids false FREQUENCY_LOW alerts
- **WHEN** `handleSystemMessages()` receives `throttleActive = 1`, `frequency = 387.5`, and `targetFrequency = 550`, followed by `throttleActive = 0`
- **THEN** `THERMAL_THROTTLE` is added when `throttleActive === 1` and removed when `throttleActive === 0`, and `FREQUENCY_LOW` is not emitted while `targetFrequency >= 400`

#### Scenario: SystemComponent renders DVFS profile and fault recovery counters
- **WHEN** `SystemComponent.getSystemRows()` receives `dvfsProfile = 3`, `watchdogResets = 2`, and `cmlErrorCount = 4`
- **THEN** the returned rows include `DVFS Profile` (`Performance`), `ASIC Watchdog Resets` (`2`), and `PMBus CML Recoveries` (`4`)
