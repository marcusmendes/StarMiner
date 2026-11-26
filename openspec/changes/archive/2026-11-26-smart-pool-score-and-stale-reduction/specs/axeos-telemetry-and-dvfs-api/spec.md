## ADDED Requirements

### Requirement: Smart Pool Failover Setting and Health Telemetry in API and OpenAPI
The system SHALL persist `stratumSmartFailover` (`NVS_CONFIG_STRATUM_SMART_FAILOVER`, default `false`) via `PATCH /api/system`, expose `poolHealthScore`, `poolRttEwmaMs`, `poolStaleRatePct`, `staleSharesSuppressed`, `smartFailoverCount`, and `stratumSmartFailover` in `GET /api/system/info` and `openapi.yaml`, and keep `system.service.ts` mocks synchronized.

#### Scenario: PATCH /api/system persists stratumSmartFailover setting
- **WHEN** a client sends `PATCH /api/system` with `{"stratumSmartFailover": true}`
- **THEN** NVS stores `NVS_CONFIG_STRATUM_SMART_FAILOVER = true` and `GET /api/system/info` reflects the updated setting

#### Scenario: GET /api/system/info returns pool health and stale suppression metrics
- **WHEN** a client requests `GET /api/system/info` during mining
- **THEN** the response includes `poolHealthScore` (`0..100`), `poolRttEwmaMs`, `poolStaleRatePct`, `staleSharesSuppressed`, `smartFailoverCount`, and `stratumSmartFailover`

### Requirement: Pool Health Score Display and Smart Failover Toggle in AxeOS
`PoolComponent` SHALL provide an opt-in `Smart Pool Failover` toggle; `HomeComponent` SHALL display `poolHealthScore` and stale/suppressed telemetry in the Pool card; and `SwarmComponent` SHALL display a pool health badge while tolerating legacy firmware lacking those fields.

#### Scenario: AxeOS Pool, Home, and Swarm render smart failover controls and health metrics
- **WHEN** `SystemInfo` includes `stratumSmartFailover`, `poolHealthScore = 92`, `poolRttEwmaMs = 138.4`, `poolStaleRatePct = 0.85`, and `staleSharesSuppressed = 4`
- **THEN** `PoolComponent` binds the `stratumSmartFailover` control, `HomeComponent` displays the `92/100` pool health score and suppressed stale count, and `SwarmComponent` renders the health badge safely even if peer devices omit the new fields
