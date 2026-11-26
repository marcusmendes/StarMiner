## ADDED Requirements

### Requirement: Pool Health Score and Stale Suppression Unit Tests in C
The `components/stratum/test/test_pool_score.c` Unity suite SHALL verify EWMA RTT tracking, coin-normalized stale penalties (`15 s` DGB vs `600 s` BTC), disconnect decay, failover hysteresis/cooldown gating, and `job_epoch` stale suppression (including block-candidate passthrough) in `test` / `test-ci` / QEMU.

#### Scenario: Unity suite validates scoring bounds, coin normalization, failover hysteresis, and epoch suppression
- **WHEN** `test` / `test-ci` executes `components/stratum/test/test_pool_score.c`
- **THEN** assertions verify score stays clamped in `0..100`, DGB vs BTC stale tolerance behaves distinctly, failover requires minimum samples plus streak plus cooldown, and obsolete non-block shares are suppressed while `nonce_diff >= network_nonce_diff` candidates pass through

### Requirement: AxeOS Unit Tests for Smart Failover and Pool Health Telemetry
The frontend Jasmine suites (`pool.component.spec.ts`, `home.component.spec.ts`) SHALL verify `stratumSmartFailover` form control binding and pool health telemetry rendering under `npm run test:ci`.

#### Scenario: Frontend CI validates smart failover control and pool health display
- **WHEN** `npm run test:ci` executes in `main/http_server/axe-os`
- **THEN** `pool.component.spec.ts` asserts `stratumSmartFailover` form state and payload updates, `home.component.spec.ts` asserts `poolHealthScore` / `staleSharesSuppressed` rendering, and `system.service.ts` mocks satisfy regenerated `SystemInfo` types
