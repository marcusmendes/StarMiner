## Purpose

Define o registro de parâmetros de moedas SHA-256, auto-detecção e override por pool, decodificação nativa de endereços e payouts da transação coinbase, e restrição de sinalizações exclusivas do Bitcoin.

## ADDED Requirements

### Requirement: SHA-256 Coin Registry and Auto-Detection
The system SHALL support `Auto (0)` plus explicit SHA-256 coin profiles (`1=BTC`, `2=DGB`, `3=BCH`, `4=BSV`, `5=FB`, `6=BC2`, `7=XEC`, `8=PPC`, `9=NMC`, `10=Generic SHA-256`) with `ticker`, `decimals`, `block_time_s`, `p2pkh_version`, `p2sh_version`, and `bech32_hrp`, auto-detecting from `stratumUser` Bech32 HRP or Base58Check version byte and `stratumURL` hints when set to `Auto`.

#### Scenario: Auto mode resolves DigiByte from P2PKH or Bech32 user address
- **WHEN** the active coin profile is `0` (`Auto`) and `stratumUser` starts with a `DGB` P2PKH address (version byte `0x1E`, e.g. `D...`) or `dgb1` Bech32 address (ignoring `.worker` suffixes)
- **THEN** the resolver selects `DGB` (`ticker = "DGB"`, `decimals = 8`, `block_time_s = 15`, `p2pkh_version = 0x1E`, `p2sh_version = 0x3F`, `bech32_hrp = "dgb"`, `user_is_address = true`)

#### Scenario: Explicit pool coin profile overrides auto-detection
- **WHEN** `stratumCoinProfile` (or `fallbackStratumCoinProfile`) is configured to an explicit non-zero profile (`1..10`)
- **THEN** the resolver uses that coin's parameters regardless of `stratumURL` hints or shared prefix bytes

#### Scenario: Non-address pool username or unknown chain falls back safely
- **WHEN** `stratumCoinProfile` is `0` (`Auto`) and `stratumUser` is a pool account username without a valid address (or resolves to `Generic SHA-256`)
- **THEN** the resolver marks `user_is_address = false` and avoids false coinbase payout mismatch diagnostics

### Requirement: Native Multi-Coin Coinbase Decoding and Payload Payout Matching
`coinbase_process_notification` SHALL decode `P2PKH`, `P2SH`, `P2WPKH`, `P2WSH`, and `P2TR` outputs using the resolved coin's native version bytes and Bech32 HRP, match user payouts by decoded address or underlying script payload (after stripping `.worker` suffixes), and populate `is_user_output` and `user_value_satoshis`.

#### Scenario: Solo DGB coinbase decodes native addresses and credits user payout share
- **WHEN** `coinbase_process_notification` processes a DigiByte solo template paying 99% to a `D...` P2PKH output and 1% to a `dgb1...` pool fee output
- **THEN** the decoded outputs report the native `D...` and `dgb1...` strings, `is_user_output` is `true` for the user's output, and `user_value_satoshis` equals 99% of `total_value_satoshis`

#### Scenario: Raw script payload matching identifies user payout across encodings
- **WHEN** `stratumUser` contains a valid P2PKH/P2SH/SegWit address whose underlying `hash160` or witness program matches a coinbase output's `scriptPubKey`
- **THEN** `is_user_output` evaluates to `true` even if the address prefix bytes or encoding casing differ

### Requirement: Bitcoin-Only BIP Signaling Gating
The coinbase decoder SHALL evaluate and emit `BIP-54` and `BIP-110` signaling flags ONLY when the active resolved coin profile belongs to the Bitcoin family (`BTC`), suppressing Bitcoin soft-fork badges on altcoins.

#### Scenario: Altcoin block template suppresses BTC-only BIP signals
- **WHEN** `coinbase_process_notification` processes a block template resolved as `DGB` (or any non-BTC coin) whose version or `nLockTime` happens to match Bitcoin BIP-54 or BIP-110 patterns
- **THEN** `bip54_signaling` and `bip110_signaling` remain `false` and no Bitcoin BIP badges are emitted

#### Scenario: Bitcoin template preserves BIP-54 and BIP-110 signaling detection
- **WHEN** the active resolved coin is `BTC` and `decode_coinbase_tx` is enabled on a template signaling BIP-54 or BIP-110
- **THEN** the corresponding `bip54_signaling` / `bip110_signaling` flags and `blockSignals` entries are emitted normally
