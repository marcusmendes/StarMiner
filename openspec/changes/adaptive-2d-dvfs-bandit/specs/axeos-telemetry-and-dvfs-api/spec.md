## MODIFIED Requirements

### Requirement: Extended Telemetry Fields in System Info JSON
The system SHALL include `throttleActive` (`0` or `1`), `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile` (`0..4`), `cmlErrorCount`, `watchdogResets`, `preWatchdogRecoveries`, `adaptiveBestFreq`, and `adaptiveBestVcore` in `GET /api/system/info` while reporting effective RAM frequency in `frequency` and effective commanded RAM VCore in `coreVoltage`.

#### Scenario: GET /api/system/info returns effective RAM and NVS target telemetry
- **WHEN** a client requests `GET /api/system/info` (or receives a WebSocket state broadcast via `system_api_get_full_json`)
- **THEN** the JSON payload includes `throttleActive` (`0` or `1`), `targetFrequency` (NVS target MHz), `targetVoltage` (NVS target mV), `frequency` (effective RAM MHz), `coreVoltage` (effective commanded RAM mV), `vdroopOffsetMv` (applied Vdroop offset in mV), `dvfsProfile` (`0..4`), `cmlErrorCount`, `watchdogResets`, `preWatchdogRecoveries`, `adaptiveBestFreq`, and `adaptiveBestVcore`

### Requirement: DVFS Profile Persistence and PATCH API Support
The system SHALL persist `dvfsProfile` (`NVS_CONFIG_DVFS_PROFILE`, key `"dvfsprofile"`, default `0`, range `0..4`) in NVS and accept updates via `PATCH /api/system`.

#### Scenario: Updating dvfsProfile via PATCH /api/system
- **WHEN** a client sends `PATCH /api/system` with `{"dvfsProfile": 4}` (value between `0` and `4`)
- **THEN** the backend validates the range `0..4`, saves `NVS_CONFIG_DVFS_PROFILE` in NVS, and returns HTTP `200`

#### Scenario: Rejecting out-of-range dvfsProfile via PATCH /api/system
- **WHEN** a client sends `PATCH /api/system` with `dvfsProfile` outside `0..4`
- **THEN** `check_settings_and_update` rejects the payload and returns HTTP `400`

### Requirement: Target Frequency and Voltage Preservation in AxeOS Edit Form
The AxeOS Settings form (`EditComponent`) SHALL initialize its `frequency` and `coreVoltage` controls from `targetFrequency` and `targetVoltage` (falling back to `frequency` and `coreVoltage` only when target fields are absent), and SHALL provide a `dvfsProfile` selector (`0..4`) whose changes save via `PATCH /api/system` without requiring a device restart.

#### Scenario: Opening Settings during RAM soft-throttling or Vdroop preserves NVS targets
- **WHEN** `GET /api/system/info` returns `frequency = 475`, `targetFrequency = 550`, `coreVoltage = 1165`, and `targetVoltage = 1150`
- **THEN** the `EditComponent` form initializes `frequency` to `550` and `coreVoltage` to `1150` so saving unrelated settings (such as fan or display) does not overwrite the NVS targets with transient RAM values

#### Scenario: Changing dvfsProfile without triggering a restart prompt
- **WHEN** the user selects a new `dvfsProfile` value (`0 = Manual`, `1 = Efficiency`, `2 = Balanced`, `3 = Performance`, or `4 = Adaptive`) in `EditComponent` and clicks Save
- **THEN** `updateSystem()` sends `{ dvfsProfile }` in `PATCH /api/system` and `isRestartRequired` evaluates to `false`

### Requirement: Visão Comparativa de Frota (J/TH, DVFS e Alertas) na Página Swarm do AxeOS
A página `SwarmComponent` do AxeOS SHALL calcular e exibir a eficiência (`J/TH`) média da frota e individual por dispositivo, o perfil DVFS ativo (`0..4`), a frequência e tensão efetivas (com indicador de *Vdroop*), e alertas visuais para `throttleActive === 1` e `watchdogResets > 0`, avaliando frequência baixa por `(targetFrequency ?? frequency) < 400`.

#### Scenario: Frota de mineradores exibe eficiência J/TH, perfil DVFS e Vdroop lado a lado
- **WHEN** o usuário abre a página Swarm com dispositivos reportando `hashRate`, `power`, `dvfsProfile`, `frequency`, `coreVoltage` e `vdroopOffsetMv`
- **THEN** o painel exibe a eficiência agregada da frota (`J/TH`), o `J/TH` individual de cada placa, o badge do perfil DVFS (incluindo `Adaptive` para `dvfsProfile = 4`) e os valores efetivos de clock/tensão com tolerância a dispositivos legados sem esses campos

#### Scenario: Notificações do Swarm refletem throttling térmico e resets de watchdog sem falso positivo de frequência
- **WHEN** um dispositivo no Swarm reporta `throttleActive === 1` ou `watchdogResets > 0`, ou reporta `frequency < 400` enquanto `targetFrequency >= 400`
- **THEN** `getDeviceNotification()` exibe o alerta correspondente de *Thermal Throttling* ou *Watchdog* e não emite falso alerta de *Frequency Low* devido à redução transitória em RAM

## ADDED Requirements

### Requirement: Adaptive Safe Point and Pre-Watchdog Telemetry in AxeOS Home and System
The AxeOS `HomeComponent` and `SystemComponent` SHALL display `Adaptive` for `dvfsProfile = 4`, render `adaptiveBestFreq` / `adaptiveBestVcore` when available, and display `preWatchdogRecoveries` alongside watchdog and PMBus recovery counters.

#### Scenario: Home and System display Adaptive profile and pre-watchdog recovery count
- **WHEN** `SystemInfo` reports `dvfsProfile = 4`, `adaptiveBestFreq = 587.5`, `adaptiveBestVcore = 1140`, and `preWatchdogRecoveries = 2`
- **THEN** `HomeComponent` formats the DVFS profile label as `Adaptive` and `SystemComponent.getSystemRows()` includes `DVFS Profile` (`Adaptive`), the adaptive safe operating point (`587.5 MHz @ 1140 mV`), and `Pre-Watchdog Recoveries` (`2`)
