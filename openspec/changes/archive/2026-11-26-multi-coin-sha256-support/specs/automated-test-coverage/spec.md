## ADDED Requirements

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
