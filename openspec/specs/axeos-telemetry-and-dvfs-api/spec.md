# axeos-telemetry-and-dvfs-api Specification

## Purpose

Expõe os indicadores de regulação térmica em RAM, compensação de *Vdroop*, perfil DVFS, contadores de falhas `STATUS_CML` e resets de watchdog na API HTTP/WebSocket (`/api/system/info` e `PATCH /api/system`), mantendo o contrato OpenAPI (`openapi.yaml`) e os mocks do frontend Angular (`AxeOS`) sincronizados.

## Requirements

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

### Requirement: OpenAPI Schema and AxeOS Mock Parity
The project SHALL define the new telemetry and settings properties in `main/http_server/openapi.yaml`, regenerate the TypeScript client models in `main/http_server/axe-os/src/app/generated`, and include matching mock values in `main/http_server/axe-os/src/app/services/system.service.ts`.

#### Scenario: Regenerating API models and running frontend CI tests
- **WHEN** `npm run generate:api` and `npm run test:ci` (or `npm run build`) are executed in `main/http_server/axe-os`
- **THEN** the generated TypeScript interfaces (`SystemInfo` and `Settings`) and mock data in `system.service.ts` compile without TypeScript errors

### Requirement: Target Frequency and Voltage Preservation in AxeOS Edit Form
The AxeOS Settings form (`EditComponent`) SHALL initialize its `frequency` and `coreVoltage` controls from `targetFrequency` and `targetVoltage` (falling back to `frequency` and `coreVoltage` only when target fields are absent), and SHALL provide a `dvfsProfile` selector (`0..4`) whose changes save via `PATCH /api/system` without requiring a device restart.

#### Scenario: Opening Settings during RAM soft-throttling or Vdroop preserves NVS targets
- **WHEN** `GET /api/system/info` returns `frequency = 475`, `targetFrequency = 550`, `coreVoltage = 1165`, and `targetVoltage = 1150`
- **THEN** the `EditComponent` form initializes `frequency` to `550` and `coreVoltage` to `1150` so saving unrelated settings (such as fan or display) does not overwrite the NVS targets with transient RAM values

#### Scenario: Changing dvfsProfile without triggering a restart prompt
- **WHEN** the user selects a new `dvfsProfile` value (`0 = Manual`, `1 = Efficiency`, `2 = Balanced`, `3 = Performance`, or `4 = Adaptive`) in `EditComponent` and clicks Save
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

### Requirement: Persistência Pós-Reboot de Diagnóstico e Aprendizado DVFS em RTC_NOINIT
O sistema SHALL armazenar `watchdog_resets`, `cml_error_count`, `last_watchdog_reason` (`0..3`), `learned_max_freq_mhz` e `learned_min_vcore_mv` em memória `RTC_NOINIT_ATTR` protegida por `magic`, *fingerprint* dos alvos NVS e `CRC32`, restaurando-os após reinícios quentes (`esp_restart()`) sem gravação em Flash e expondo `learnedMaxFreq`, `learnedMinVcore` e `lastWatchdogReason` em `GET /api/system/info`.

#### Scenario: Reinício quente por software preserva contadores e limites DVFS aprendidos
- **WHEN** o dispositivo reinicia via `esp_restart()` (`esp_reset_reason() != ESP_RST_POWERON`) mantendo íntegros `magic`, `CRC32` e os valores configurados na NVS (`target_frequency`, `target_voltage`, `dvfs_profile`)
- **THEN** o sistema restaura `watchdog_resets`, `cml_error_count`, `last_watchdog_reason`, `learned_max_freq_mhz` e `learned_min_vcore_mv` da RAM RTC e os expõe em `/api/system/info`

#### Scenario: Cold boot ou alteração de alvo NVS descarta estado RTC anterior
- **WHEN** o dispositivo inicia por *power-on* (`ESP_RST_POWERON`), apresenta `CRC32` inválido ou detecta mudança nos alvos da NVS frente ao *fingerprint* armazenado
- **THEN** o sistema inicializa os contadores e limites aprendidos zerados com segurança sem corromper o estado operacional

### Requirement: Visão Comparativa de Frota (J/TH, DVFS e Alertas) na Página Swarm do AxeOS
A página `SwarmComponent` do AxeOS SHALL calcular e exibir a eficiência (`J/TH`) média da frota e individual por dispositivo, o perfil DVFS ativo (`0..4`), a frequência e tensão efetivas (com indicador de *Vdroop*), e alertas visuais para `throttleActive === 1` e `watchdogResets > 0`, avaliando frequência baixa por `(targetFrequency ?? frequency) < 400`.

#### Scenario: Frota de mineradores exibe eficiência J/TH, perfil DVFS e Vdroop lado a lado
- **WHEN** o usuário abre a página Swarm com dispositivos reportando `hashRate`, `power`, `dvfsProfile`, `frequency`, `coreVoltage` e `vdroopOffsetMv`
- **THEN** o painel exibe a eficiência agregada da frota (`J/TH`), o `J/TH` individual de cada placa, o badge do perfil DVFS (incluindo `Adaptive` para `dvfsProfile = 4`) e os valores efetivos de clock/tensão com tolerância a dispositivos legados sem esses campos

#### Scenario: Notificações do Swarm refletem throttling térmico e resets de watchdog sem falso positivo de frequência
- **WHEN** um dispositivo no Swarm reporta `throttleActive === 1` ou `watchdogResets > 0`, ou reporta `frequency < 400` enquanto `targetFrequency >= 400`
- **THEN** `getDeviceNotification()` exibe o alerta correspondente de *Thermal Throttling* ou *Watchdog* e não emite falso alerta de *Frequency Low* devido à redução transitória em RAM

### Requirement: Adaptive Safe Point and Pre-Watchdog Telemetry in AxeOS Home and System
The AxeOS `HomeComponent` and `SystemComponent` SHALL display `Adaptive` for `dvfsProfile = 4`, render `adaptiveBestFreq` / `adaptiveBestVcore` when available, and display `preWatchdogRecoveries` alongside watchdog and PMBus recovery counters.

#### Scenario: Home and System display Adaptive profile and pre-watchdog recovery count
- **WHEN** `SystemInfo` reports `dvfsProfile = 4`, `adaptiveBestFreq = 587.5`, `adaptiveBestVcore = 1140`, and `preWatchdogRecoveries = 2`
- **THEN** `HomeComponent` formats the DVFS profile label as `Adaptive` and `SystemComponent.getSystemRows()` includes `DVFS Profile` (`Adaptive`), the adaptive safe operating point (`587.5 MHz @ 1140 mV`), and `Pre-Watchdog Recoveries` (`2`)
