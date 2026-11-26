## ADDED Requirements

### Requirement: Adaptive 2D Bandit Unit Tests in C
The `components/asic/test/` Unity suite SHALL test the pure C 2D Contextual Bandit controller (`dvfs_bandit`), covering grid indexing, hard caps (`400.0–675.0 MHz`, `1000–1250 mV`), reward computation, hysteresis, temporal decay, pre-watchdog/watchdog rollback and cell penalty, and updated `PcmRtcDiagnostics` CRC32 round-trip.

#### Scenario: Bandit grid convergence, penalties, and rollback pass Unity tests
- **WHEN** `test` / `test-ci` executes the `dvfs_bandit` and `power_control` Unity test cases
- **THEN** assertions verify that step commands respect grid step sizes and hard caps, pre-watchdog/watchdog feedback rolls back to the last proven safe point and penalizes the fault cell, temporal discounting decays stale observations, and `PcmRtcDiagnostics` serialization preserves `adaptive_best_freq_mhz` and `adaptive_best_vcore_mv` across CRC validation

### Requirement: AxeOS Unit Tests for Adaptive Profile and Extended Telemetry
The frontend Jasmine suites (`edit.component.spec.ts`, `home.component.spec.ts`, `system.component.spec.ts`) SHALL test `dvfsProfile = 4` (`Adaptive`) selection, validation up to `4`, label rendering, and display of `preWatchdogRecoveries` and adaptive safe-point telemetry.

#### Scenario: Frontend CI passes with dvfsProfile 4 and extended telemetry mocks
- **WHEN** `npm run test:ci` runs in `main/http_server/axe-os`
- **THEN** tests verify `EditComponent` accepts `dvfsProfile = 4`, `HomeComponent` formats `dvfsProfile = 4` as `Adaptive`, `SystemComponent.getSystemRows()` exposes `Pre-Watchdog Recoveries` and adaptive safe-point values, and `system.service.ts` mocks satisfy the regenerated OpenAPI `SystemInfo` type
