# Tasks: Suporte Multi-Moeda SHA-256 (`Auto`, `BTC`, `DGB`, `BCH`, `BSV`, `FB`, `BC2`, `XEC`, `PPC`, `NMC`, `Generic SHA-256`)

## 1. Registro multi-moeda e decodificador de coinbase em C (`components/stratum`)

- [x] 1.1 Criar `components/stratum/include/coin_params.h` e `components/stratum/coin_params.c` com `CoinProfileId` (`0..10`), tabela de parâmetros (`BTC`, `DGB`, `BCH`, `BSV`, `FB`, `BC2`, `XEC`, `PPC`, `NMC`, `GENERIC`) e `coin_resolve_params()` (auto-detecção por HRP Bech32, byte Base58Check e hints de `stratumURL` + validação de `user_is_address`), registrando `coin_params.c` em `components/stratum/CMakeLists.txt` e validando compilação do componente `stratum`.
- [x] 1.2 Atualizar `components/stratum/include/coinbase_decoder.h` e `components/stratum/coinbase_decoder.c` com `coinbase_decode_address_with_params()`, `coinbase_process_notification_ex()`, comparação dual de payout (string + payload binário de `scriptPubKey` sem sufixo `.worker`), preenchimento de `resolved_coin` / `user_is_address` e gate de `bip54_signaling`/`bip110_signaling` restrito à família Bitcoin.
- [x] 1.3 Ampliar `components/stratum/test/test_coinbase_decoder.c` cobrindo auto-detecção e overrides (`DGB` `0x1E`/`0x3F`/`dgb1`, `BTC`, `PPC`, `NMC`, `XEC`, `Generic`), decodificação nativa DGB e cálculo de `user_value_satoshis`, casamento por payload bruto, usuário não-endereço e supressão de `BIP-54`/`BIP-110` fora do Bitcoin, verificando 100% de aprovação via `./tools/run_qemu_tests.sh`.

## 2. Integração em NVS, `GlobalState` e tasks Stratum v1/v2

- [x] 2.1 Adicionar `NVS_CONFIG_STRATUM_COIN_PROFILE` (`"stratumcoin"`, default `0`, `0..10`) e `NVS_CONFIG_FALLBACK_STRATUM_COIN_PROFILE` (`"fbstratumcoin"`, default `0`, `0..10`) em `main/nvs_config.{h,c}` e carregar os valores em `SystemModule` / `GlobalState` (`main/global_state.h`).
- [x] 2.2 Atualizar `main/tasks/stratum_v1_task.c` e `main/tasks/stratum_v2_task.c` para chamar `coinbase_process_notification_ex()` passando URL/perfil da pool ativa (primária ou fallback), propagando `coin_ticker`, `coin_decimals`, `coin_block_time_s` e `coin_user_is_address` para `GlobalState` e filtrando `BIP-54`/`BIP-110` em `block_signals`, verificando compilação completa com `idf.py build`.

## 3. API REST, OpenAPI e mocks do AxeOS

- [x] 3.1 Atualizar `main/http_server/system_api_json.c` para expor `coinTicker`, `coinDecimals`, `coinBlockTimeSeconds`, `coinUserIsAddress` (`0`/`1`), `stratumCoinProfile`, `fallbackStratumCoinProfile` e `isUserOutput` (booleano por saída em `coinbaseOutputs`), validando o payload de `GET /api/system/info` e `PATCH /api/system`.
- [x] 3.2 Atualizar `main/http_server/openapi.yaml` com os novos campos em `SystemInfo`, `coinbaseOutputs` e `Settings` (`0..10`), executar `npm run generate:api` em `main/http_server/axe-os` e atualizar os mocks em `main/http_server/axe-os/src/app/services/system.service.ts`, verificando compilação limpa via `npm run build`.

## 4. Interface AxeOS (`SatsPipe`, `Home`, `Pool`) e testes Jasmine

- [x] 4.1 Atualizar `main/http_server/axe-os/src/app/pipes/sats.pipe.ts` para aceitar `(value, ticker = 'BTC', decimals = 8)` mantendo compatibilidade retroativa com chamadas existentes, e adicionar casos para `BTC`, `DGB` (`8` decimais) e `XEC` (`2` decimais) em `sats.pipe.spec.ts`.
- [x] 4.2 Atualizar `HomeComponent` (`home.component.{ts,html}`) para exibir valores com `info.coinTicker` / `info.coinDecimals`, destacar `output.isUserOutput || output.address === activePoolUserAddressPart` com ★, ajustar tooltips BTC-only e suprimir `NO_MINING_REWARD` / `NOT_SOLO_MINING` quando `coinUserIsAddress === 0` ou `coinTicker === 'SHA256'`.
- [x] 4.3 Atualizar `PoolComponent` (`pool.component.{ts,html}`) com dropdown de moeda (`0=Auto..10=Generic SHA-256`) para pool primária e fallback e revisar avisos/tooltips BTC-only, ampliando `home.component.spec.ts` e `pool.component.spec.ts` e validando tudo com `npm run test:ci` em `main/http_server/axe-os`.

## 5. Verificação integrada e validação em hardware DGB

- [x] 5.1 Executar a bateria integrada final antes do deploy (`./tools/run_qemu_tests.sh`, `npm run test:ci` em `main/http_server/axe-os` e `idf.py build`), confirmando zero falhas ou regressões na suíte de Bitcoin existente.
- [x] 5.2 Realizar flash OTA (`esp-miner.bin` + `www.bin`) na frota em `lat-dgb.solofury.com` e validar em `/api/system/info` e no AxeOS: `coinTicker = "DGB"`, `coinBlockTimeSeconds = 15`, endereços nativos (`DJKYme7v...` ★ + `dgb1q...`), `coinbaseValueUserSatoshis` ≈ 99% do bloco, valores em `DGB`, ausência de `NO_MINING_REWARD` e ausência de sinais `BIP-54`/`BIP-110` espúrios.

## Workflow follow-up

- Após implementar e validar em hardware DGB, arquivar esta change via `/openspec-archive-change` (mesclando `multi-coin-sha256-decoding`, `axeos-telemetry-and-dvfs-api` e `automated-test-coverage` para `openspec/specs/`).
- Com `coinBlockTimeSeconds` já disponível em `GlobalState` e `SystemInfo`, prosseguir para a proposta da Fase 3A (`smart-pool-score-and-stale-reduction`).
