## ADDED Requirements

### Requirement: Multi-Coin Pool Configuration and Telemetry in API and OpenAPI
The system SHALL persist `stratumCoinProfile` (`NVS_CONFIG_STRATUM_COIN_PROFILE`) and `fallbackStratumCoinProfile` (`NVS_CONFIG_FALLBACK_STRATUM_COIN_PROFILE`, default `0`, range `0..10`) via `PATCH /api/system`, expose `coinTicker`, `coinDecimals`, `coinBlockTimeSeconds`, `coinUserIsAddress`, and `coinbaseOutputs[].isUserOutput` in `GET /api/system/info` and `openapi.yaml`, and keep `system.service.ts` mocks synchronized.

#### Scenario: Updating stratumCoinProfile via PATCH /api/system validates 0..10 range
- **WHEN** a client sends `PATCH /api/system` with `{"stratumCoinProfile": 2}` (`0..10`) or an out-of-range value (`> 10`)
- **THEN** valid profiles persist in NVS and return HTTP `200`, while out-of-range values return HTTP `400`

#### Scenario: GET /api/system/info returns active coin metadata and user output flag
- **WHEN** mining DigiByte (`DGB`) with coinbase decoding enabled
- **THEN** `GET /api/system/info` includes `coinTicker: "DGB"`, `coinDecimals: 8`, `coinBlockTimeSeconds: 15`, `coinUserIsAddress: 1`, `stratumCoinProfile`, `fallbackStratumCoinProfile`, and `isUserOutput` (`true`/`false`) per entry in `coinbaseOutputs`

### Requirement: Multi-Coin Formatting, Pool Selector, and Warning Suppression in AxeOS
`SatsPipe` SHALL format coinbase values using active `coinTicker` and `coinDecimals`; `PoolComponent` SHALL expose coin selectors (`0..10`) for primary and fallback pools; and `HomeComponent` SHALL highlight `output.isUserOutput`, display accurate coin units, and suppress false `NO_MINING_REWARD` / `NOT_SOLO_MINING` alerts for non-address pool usernames or `Generic SHA-256`.

#### Scenario: DGB solo mining renders native DGB units without false reward warnings
- **WHEN** `SystemInfo` reports `coinTicker = "DGB"`, `coinDecimals = 8`, `coinUserIsAddress = 1`, `coinbaseValueTotalSatoshis = 24516336428`, and `coinbaseValueUserSatoshis = 24271173064`
- **THEN** `HomeComponent` renders `245.16336428 DGB`, stars the output with `isUserOutput === true`, and emits no `NO_MINING_REWARD` warning

#### Scenario: Non-address username or Generic SHA-256 suppresses payout share warnings
- **WHEN** `SystemInfo` reports `coinUserIsAddress = 0` or `coinTicker = "SHA256"` while `coinbaseOutputs` is non-empty
- **THEN** `HomeComponent.handleSystemMessages()` clears and suppresses `NO_MINING_REWARD` and `NOT_SOLO_MINING` alerts, and `SatsPipe` formats `XEC` with `2` decimals (`0.00 XEC` scaling) when `coinDecimals = 2`
