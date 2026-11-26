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

### Requirement: Testes unitários em C para filtro CUSUM de domínios e alívio preventivo a quente
A suíte Unity em `components/asic/test/test_power_control.c` (executada em `test` / `test-ci` / QEMU) SHALL verificar imunidade do filtro `EWMA + CUSUM` a ruído curto, detecção de degradação sustentada, reset de estado e seleção de alívio preventivo a quente (`+5 mV` vs `-12.5 MHz`) nos perfis `0..3` dentro dos *Hard Caps*.

#### Scenario: Filtro CUSUM rejeita variância curta e detecta queda sustentada
- **WHEN** o teste Unity injeta séries sintéticas a `1 Hz` com quedas curtas em `~0.78` seguidas de degradação sustentada em `~0.50` em um domínio
- **THEN** as oscilações curtas não disparam detecção, a queda sustentada dispara dentro da janela esperada informando o índice do domínio degradado e `pcm_domain_cusum_reset()` zera os acumuladores

#### Scenario: Decisão de alívio a quente respeita perfis DVFS e Hard Caps
- **WHEN** o teste Unity exercita `pcm_pre_watchdog_apply_relief()` nos perfis `0..3` com e sem folga de potência, temperatura de VR e `VCore`
- **THEN** `Manual` e `Balanced` nunca elevam `VCore`, `Efficiency` e `Performance` aplicam `+5 mV` apenas dentro de `1250 mV` / `30.5 W` / `84.0 °C` e o recuo de frequência nunca ultrapassa o piso de `400.0 MHz`

### Requirement: Adaptive 2D Bandit Unit Tests in C
The `components/asic/test/` Unity suite SHALL test the v2 `dvfs_bandit` controller and `PcmRtcDiagnostics` CRC32 round-trip, covering grid indexing, hard caps, continuous power penalty, physics-prior upward convergence without downward bias, `expected_hashrate` stall fallback, hysteresis, visited-cell immunity to error ripples, temporal decay, and pre-watchdog/watchdog rollback.

#### Scenario: Bandit grid convergence, penalties, and rollback pass Unity tests
- **WHEN** `test` / `test-ci` executes the `dvfs_bandit` and `power_control` Unity test cases covering fault rollback, `~0.6–1.0%` error ripples, `>= 2.5 °C` thermal drift, and `PcmRtcDiagnostics` serialization
- **THEN** assertions verify that step commands respect grid step sizes and hard caps, pre-watchdog/watchdog feedback rolls back to the last proven safe point and penalizes the fault cell, visited cells retain their visited state across minor error ripples while real drift decays weights, and `PcmRtcDiagnostics` preserves `adaptive_best_freq_mhz` and `adaptive_best_vcore_mv` across CRC validation

#### Scenario: Bandit v2 converges upward without downward bias and penalizes VCore inflation
- **WHEN** `test` / `test-ci` exercises unvisited neighbor selection from a mid-range operating point (`600.0–625.0 MHz`) and fixed-frequency VCore sweeps below `28.0 W`
- **THEN** assertions verify the controller climbs when thermal/power/error headroom allows (rather than drifting to `500 MHz`), prefers lower wattage at equal frequency and clean error, and falls back to measured hashrate when actual output stalls well below `expected_hashrate`

### Requirement: AxeOS Unit Tests for Adaptive Profile and Extended Telemetry
The frontend Jasmine suites (`edit.component.spec.ts`, `home.component.spec.ts`, `system.component.spec.ts`) SHALL test `dvfsProfile = 4` (`Adaptive`) selection, validation up to `4`, label rendering, and display of `preWatchdogRecoveries` and adaptive safe-point telemetry.

#### Scenario: Frontend CI passes with dvfsProfile 4 and extended telemetry mocks
- **WHEN** `npm run test:ci` runs in `main/http_server/axe-os`
- **THEN** tests verify `EditComponent` accepts `dvfsProfile = 4`, `HomeComponent` formats `dvfsProfile = 4` as `Adaptive`, `SystemComponent.getSystemRows()` exposes `Pre-Watchdog Recoveries` and adaptive safe-point values, and `system.service.ts` mocks satisfy the regenerated OpenAPI `SystemInfo` type

### Requirement: Multi-Coin Coinbase Decoder Unit Tests in C
The `components/stratum/test/` Unity suite SHALL test `coin_params` auto-detection, explicit per-pool overrides, native DGB decoding (`0x1E`/`0x3F`/`dgb1`), raw script payload payout matching, and Bitcoin-only `BIP-54` / `BIP-110` gating in `test` / `test-ci` / QEMU.

#### Scenario: DGB coinbase fixture and BIP gating pass Unity tests
- **WHEN** `test` / `test-ci` executes the multi-coin cases in `components/stratum/test/test_coinbase_decoder.c`
- **THEN** assertions verify `Auto` resolves `DGB` (`15 s`, `0x1E`/`0x3F`, `dgb`), native `D...` and `dgb1...` outputs decode with accurate `user_value_satoshis` and `is_user_output`, raw payload matching credits alternate address encodings, `bip54_signaling` / `bip110_signaling` stay `false` for non-BTC coins, and non-address usernames set `user_is_address = false`

### Requirement: AxeOS Unit Tests for Multi-Coin Formatting and Warnings
The frontend Jasmine suites (`sats.pipe.spec.ts`, `home.component.spec.ts`, `pool.component.spec.ts`) SHALL verify `SatsPipe` multi-ticker and variable-decimal formatting, `HomeComponent` suppression of false payout alerts for `DGB` and non-address/`Generic SHA-256` users, and `PoolComponent` coin profile form controls under `npm run test:ci`.

#### Scenario: Frontend CI verifies BTC, DGB, XEC formatting and multi-coin alerts
- **WHEN** `npm run test:ci` runs in `main/http_server/axe-os`
- **THEN** tests confirm `SatsPipe` formats `BTC` (8 decimals), `DGB` (8 decimals), and `XEC` (2 decimals), `HomeComponent` emits no `NO_MINING_REWARD` warning for `DGB` payouts or `coinUserIsAddress = 0`, and `PoolComponent` includes `stratumCoinProfile` / `fallbackStratumCoinProfile`

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
