# Proposal: Suporte Multi-Moeda SHA-256 (`Auto`, `BTC`, `DGB`, `BCH`, `BSV`, `FB`, `BC2`, `XEC`, `PPC`, `NMC`, `Generic SHA-256`)

## Why

Atualmente o decodificador de coinbase (`components/stratum/coinbase_decoder.c`) e a interface do AxeOS (`SatsPipe`, `HomeComponent`, `PoolComponent`) assumem apenas prefixos, sinalizações BIP e unidades do Bitcoin. Ao minerar outras moedas SHA-256 — como DigiByte (`DGB`) em `lat-dgb.solofury.com` — os endereços são codificados incorretamente com prefixo BTC (`1...` e `bc1...`), fazendo `coinbaseValueUserSatoshis` zerar, disparando o alerta falso `"You don't have a share in the mining reward"` (`NO_MINING_REWARD`) e exibindo recompensas em `BTC` em vez de `DGB`.

## What Changes

- **Registro multi-moeda SHA-256 em C puro (`coin_params`)**: catálogo de perfis (`0=Auto`, `1=BTC`, `2=DGB`, `3=BCH`, `4=BSV`, `5=FB`, `6=BC2`, `7=XEC`, `8=PPC`, `9=NMC`, `10=Generic SHA-256`) com bytes de versão P2PKH/P2SH, HRP Bech32, ticker, casas decimais e tempo alvo de bloco (`block_time_s`).
- **Auto-detecção + override por pool**: inferência automática via endereço em `stratumUser` (Base58Check version byte / Bech32 HRP) e heurística de `stratumURL`, com possibilidade de override manual por pool (`stratumCoinProfile` e `fallbackStratumCoinProfile`) persistido em NVS.
- **Decodificação nativa e casamento por payload na coinbase**: `coinbase_decoder` passa a codificar endereços nativos da moeda ativa, comparar o payout do usuário tanto por string quanto pelo payload binário subjacente do `scriptPubKey` (após remover sufixos `.worker`) e restringir sinalizações `BIP-54`/`BIP-110` apenas à família Bitcoin.
- **API OpenAPI e UX multi-moeda no AxeOS**: exposição de `coinTicker`, `coinDecimals`, `coinBlockTimeSeconds`, `coinUserIsAddress`, `stratumCoinProfile`, `fallbackStratumCoinProfile` e `coinbaseOutputs[].isUserOutput` em `/api/system/info`, formatação dinâmica em `SatsPipe`, seletor de moeda em `PoolComponent` e supressão de falsos avisos BTC-only em `HomeComponent`.
- **Testes automatizados**: casos Unity em `components/stratum/test/test_coinbase_decoder.c` e specs Jasmine para `SatsPipe`, `HomeComponent` e `PoolComponent`.

## Capabilities

### New Capabilities

- `multi-coin-sha256-decoding`: Registro de parâmetros de moedas SHA-256 (`Auto`, `BTC`, `DGB`, `BCH`, `BSV`, `FB`, `BC2`, `XEC`, `PPC`, `NMC`, `Generic SHA-256`), auto-detecção por endereço/URL com override por pool, decodificação nativa de outputs da coinbase, casamento de payout por endereço/payload bruto e filtragem de sinalização BIP exclusiva do Bitcoin.

### Modified Capabilities

- `axeos-telemetry-and-dvfs-api`: Persistência em NVS e API OpenAPI para `stratumCoinProfile` / `fallbackStratumCoinProfile` (`0..10`), exposição de `coinTicker`, `coinDecimals`, `coinBlockTimeSeconds`, `coinUserIsAddress` e `coinbaseOutputs[].isUserOutput` em `SystemInfo`, formatação dinâmica de ticker/casas decimais em `SatsPipe`, remoção de falsos avisos BTC-only em `HomeComponent` e seletor de moeda em `PoolComponent`.
- `automated-test-coverage`: Testes Unity em `components/stratum/test/test_coinbase_decoder.c` cobrindo registro/auto-detecção multi-moeda, DGB (`0x1E`/`0x3F`/`dgb1`), casamento por payload e gate de BIP-54/BIP-110, além de specs Jasmine em `sats.pipe.spec.ts`, `home.component.spec.ts` e `pool.component.spec.ts`.

## Impact

- **Stratum & Coinbase C (`components/stratum/`)**: novo `coin_params.{h,c}`, evolução compatível em `coinbase_decoder.{h,c}`, `CMakeLists.txt` e `test/test_coinbase_decoder.c`.
- **NVS, Estado & Tasks (`main/`)**: `nvs_config.{h,c}`, `global_state.h`, `tasks/stratum_v1_task.c` e `tasks/stratum_v2_task.c`.
- **HTTP/OpenAPI & AxeOS (`main/http_server/`)**: `system_api_json.c`, `openapi.yaml` (regeneração via `npm run generate:api`), `system.service.ts` mocks, `sats.pipe.{ts,spec.ts}`, `home.component.{ts,html,spec.ts}` e `pool.component.{ts,html,spec.ts}`.
- **Integração futura (Fase 3A)**: A exposição de `coinBlockTimeSeconds` (`15 s` em DGB vs. `600 s` em BTC) prepara a base de normalização de *stale rate* para a futura change `smart-pool-score-and-stale-reduction`.
