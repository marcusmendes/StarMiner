# Spec Delta

## Purpose

Expõe os indicadores de regulação térmica em RAM, compensação de *Vdroop*, perfil DVFS, contadores de falhas `STATUS_CML` e resets de watchdog na API HTTP/WebSocket (`/api/system/info` e `PATCH /api/system`), mantendo o contrato OpenAPI (`openapi.yaml`) e os mocks do frontend Angular (`AxeOS`) sincronizados.

## ADDED Requirements

### Requirement: Extended Telemetry Fields in System Info JSON
The system SHALL include `throttleActive` (`0` or `1`), `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount`, and `watchdogResets` in `GET /api/system/info` while reporting effective RAM frequency in `frequency` and effective commanded RAM VCore in `coreVoltage`.

#### Scenario: GET /api/system/info returns effective RAM and NVS target telemetry
- **WHEN** a client requests `GET /api/system/info` (or receives a WebSocket state broadcast via `system_api_get_full_json`)
- **THEN** the JSON payload includes `throttleActive` (`0` or `1`), `targetFrequency` (NVS target MHz), `targetVoltage` (NVS target mV), `frequency` (effective RAM MHz), `coreVoltage` (effective commanded RAM mV), `vdroopOffsetMv` (applied Vdroop offset in mV), `dvfsProfile` (`0..3`), `cmlErrorCount`, and `watchdogResets`

### Requirement: DVFS Profile Persistence and PATCH API Support
The system SHALL persist `dvfsProfile` (`NVS_CONFIG_DVFS_PROFILE`, key `"dvfsprofile"`, default `0`, range `0..3`) in NVS and accept updates via `PATCH /api/system`.

#### Scenario: Updating dvfsProfile via PATCH /api/system
- **WHEN** a client sends `PATCH /api/system` with `{"dvfsProfile": 2}` (value between `0` and `3`)
- **THEN** the backend validates the range `0..3`, saves `NVS_CONFIG_DVFS_PROFILE` in NVS, and returns HTTP `200`

#### Scenario: Rejecting out-of-range dvfsProfile via PATCH /api/system
- **WHEN** a client sends `PATCH /api/system` with `dvfsProfile` outside `0..3`
- **THEN** `check_settings_and_update` rejects the payload and returns HTTP `400`

### Requirement: OpenAPI Schema and AxeOS Mock Parity
The project SHALL define the new telemetry and settings properties in `main/http_server/openapi.yaml`, regenerate the TypeScript client models in `main/http_server/axe-os/src/app/generated`, and include matching mock values in `main/http_server/axe-os/src/app/services/system.service.ts`.

#### Scenario: Regenerating API models and running frontend CI tests
- **WHEN** `npm run generate:api` and `npm run test:ci` (or `npm run build`) are executed in `main/http_server/axe-os`
- **THEN** the generated TypeScript interfaces (`SystemInfo` and `Settings`) and mock data in `system.service.ts` compile without TypeScript errors
