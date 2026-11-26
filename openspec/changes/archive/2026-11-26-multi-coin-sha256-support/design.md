# Design: Suporte Multi-Moeda SHA-256 (`Auto`, `BTC`, `DGB`, `BCH`, `BSV`, `FB`, `BC2`, `XEC`, `PPC`, `NMC`, `Generic SHA-256`)

## Context

Veja [proposal.md](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/openspec/changes/multi-coin-sha256-support/proposal.md).

Evidência observada em campo ([20261126-085811/raw.jsonl](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/backups/telemetry/20261126-085811/raw.jsonl)):
- Minerando DGB solo em `lat-dgb.solofury.com` com `stratumUser = DJKYme7vLUcgDAJGqjXbcmqdRVWnH5xQnU.Bitaxe01`, [coinbase_decoder.c](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/components/stratum/coinbase_decoder.c#L61-L62) força `p2pkh = 0x00`, `p2sh = 0x05` e `bech32_hrp = "bc"`, convertendo a saída DGB do usuário em `1EBTEPBH34iPgA7g79Y351g2YMnUwsZ1Cp` e a taxa de `1%` da pool em `bc1qvayz...`.
- Como a string resultante difere de `DJKYme7v...`, `coinbaseValueUserSatoshis` vai para `0`, disparando `NO_MINING_REWARD` em [home.component.ts](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/main/http_server/axe-os/src/app/components/home/home.component.ts#L1127-L1131), enquanto [sats.pipe.ts](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/main/http_server/axe-os/src/app/pipes/sats.pipe.ts) fixa `' BTC'` e as sinalizações `BIP-54` / `BIP-110` permanecem habilitadas para qualquer cadeia.

## Goals / Non-Goals

**Goals:**
- Suportar decodificação nativa, exibição de recompensa e validação de payout para múltiplas moedas SHA-256 com zero configuração manual no caso comum (`0 = Auto`), além de override explícito por pool (`1..10`).
- Eliminar completamente falsos alertas `NO_MINING_REWARD` / `NOT_SOLO_MINING` para altcoins (por comparação nativa + payload binário) e para contas nominais de pool (`user_is_address = false` ou `Generic SHA-256`).
- Expor `coinBlockTimeSeconds` desde já para servir de normalizador na Fase 3A (`smart-pool-score-and-stale-reduction`).

**Non-Goals:**
- Não implementar CashAddr completo (`bitcoincash:q...` / `ecash:q...`) no decodificador se houver preferência por Base58/Bech32; porém o casamento por payload ou reconhecimento de prefixo Bech32/Base58 + override explícito cobrem `BCH` e `XEC` com exibição correta de ticker/decimais (`8` e `2`).
- Não alterar o protocolo de submissão de shares Stratum v1/v2 (somente metadados da coinbase, NVS e camada de apresentação).

## Decisions

### 1. Módulo puro C `components/stratum/coin_params.{h,c}` com auto-detecção e override
- **Escolha**:
  - Definir enum `CoinProfileId` (`0..10`): `0=AUTO`, `1=BTC`, `2=DGB` (`15 s`, `0x1E`/`0x3F`, `"dgb"`), `3=BCH` (`600 s`, `0x00`/`0x05`), `4=BSV` (`600 s`, `0x00`/`0x05`), `5=FB` (`30 s`, `0x00`/`0x05`, `"bc"` — Fractal Bitcoin), `6=BC2` (`600 s`, `0x00`/`0x05`, `"bc"` — Bitcoin II), `7=XEC` (`600 s`, `2` decimais), `8=PPC` (`600 s`, `0x37`/`0x75`, `"pc"`), `9=NMC` (`600 s`, `0x34`/`0x0D`, `"nc"`), `10=GENERIC` (`"SHA256"`, `8` decimais, `block_time_s = 600`).
  - Struct `CoinParams` com `{ id, ticker[12], decimals, block_time_s, p2pkh_version, p2sh_version, has_bech32, bech32_hrp[8], is_btc_family }`.
  - Função `coin_resolve_params(override_profile, stratum_user, stratum_url, out_params, out_user_is_address)`:
    1. Se `override_profile` estiver em `1..10`, seleciona o perfil explícito e valida via `b58check` / `segwit_addr` se a parte antes de `.worker` / `_worker` em `stratum_user` é endereço válido (`out_user_is_address`).
    2. Em `Auto (0)`: extrai a parte de endereço (antes de `.` ou `_`, quando aplicável), tenta decodificar Bech32 HRP (`"bc"` → `BTC`, `"dgb"` → `DGB`, `"pc"` → `PPC`, `"nc"` → `NMC`, `"tb"`/`"bcrt"` → testnet `BTC`) e depois Base58Check (`0x1E`/`0x3F` → `DGB`, `0x37`/`0x75` → `PPC`, `0x34`/`0x0D` → `NMC`, `0x00`/`0x05` → `BTC` ou refina por dica em `stratum_url` como `bch`/`bsv`/`fb`/`fractal`/`xec`/`bc2`). Se `stratum_user` não for endereço válido, refina apenas por URL ou cai em `GENERIC` com `out_user_is_address = false`.
- **Alternativas consideradas**:
  - Inferir apenas no frontend Angular: deixaria `/api/system/info`, logs da MCU e `coinbaseValueUserSatoshis` errados e acoplaria conhecimento de scriptPubKey ao browser.

### 2. Evolução retrocompatível de `coinbase_decoder.{h,c}`
- **Escolha**:
  - Adicionar `coinbase_decode_address_with_params(script, script_len, output, output_len, const CoinParams *params)` mantendo `coinbase_decode_address_from_scriptpubkey(...)` como wrapper para compatibilidade integral com os testes atuais.
  - Adicionar `coinbase_process_notification_ex(..., const char *stratum_url, CoinProfileId coin_profile, mining_notification_result_t *result)` e estender `mining_notification_result_t` com `CoinParams resolved_coin` e `bool user_is_address`. Manter `coinbase_process_notification(...)` como wrapper passando `stratum_url = NULL`, `coin_profile = COIN_PROFILE_AUTO`.
  - Para identificar `is_user_output`: primeiro extrair a parte endereço de `user_address` (antes de `.worker`), comparar com `output_address` decodificado **e** extrair o `hash160` (20 B) ou programa SegWit (20/32 B) do `user_address` via `b58tobin` / `segwit_addr_decode` para comparar diretamente contra os bytes do `scriptPubKey` (`P2PKH`, `P2SH`, `P2WPKH`, `P2WSH`, `P2TR`). Assim, mesmo que uma moeda compartilhe formato diferente, o payout do minerador continua reconhecido.
  - Restringir `result->bip54_signaling` e `result->bip110_signaling` para só serem avaliados quando `resolved_coin.is_btc_family == true`.
- **Alternativas consideradas**:
  - Reescrever toda a assinatura de `coinbase_process_notification` sem wrapper: quebraria os testes e chamadas existentes antes da migração completa.

### 3. NVS, `GlobalState`, Stratum v1/v2 e contrato OpenAPI/AxeOS
- **Escolha**:
  - `main/nvs_config.{h,c}`: adicionar `NVS_CONFIG_STRATUM_COIN_PROFILE` (`"stratumcoin"`, `TYPE_U16`, default `0`, `0..10`) e `NVS_CONFIG_FALLBACK_STRATUM_COIN_PROFILE` (`"fbstratumcoin"`, `TYPE_U16`, default `0`, `0..10`).
  - `main/global_state.h` + `SystemModule`: armazenar perfis configurados e metadados resolvidos (`coin_ticker[12]`, `coin_decimals`, `coin_block_time_s`, `coin_user_is_address`), preenchidos em `stratum_v1_task.c` e `stratum_v2_task.c` a partir de `result->resolved_coin`.
  - `main/http_server/system_api_json.c` & `openapi.yaml`: adicionar `stratumCoinProfile`, `fallbackStratumCoinProfile`, `coinTicker`, `coinDecimals`, `coinBlockTimeSeconds`, `coinUserIsAddress` (`0`/`1` seguindo o padrão numérico descrito em `AGENTS.md`) e `isUserOutput` booleano em cada objeto de `coinbaseOutputs`. Regenerar modelos TypeScript via `npm run generate:api` e sincronizar `system.service.ts`.
- **Alternativas consideradas**:
  - Guardar string de moeda em NVS: mais frágil para validação genérica em `check_settings_and_update` do que `TYPE_U16` com `min = 0, max = 10`.

### 4. Camada Angular (`SatsPipe`, `HomeComponent`, `PoolComponent`)
- **Escolha**:
  - `SatsPipe.transform(value: number, ticker: string = 'BTC', decimals: number = 8)`: divide por `10 ** decimals` (default `8`, ex. `XEC = 2`), formata com `toFixed(decimals)` e anexa `' ' + (ticker || 'BTC')`. Chamadas legadas sem argumentos continuam produzindo exatamente `'0 BTC'` / `'... BTC'`.
  - `HomeComponent`: passar `info.coinTicker` e `info.coinDecimals` para `| sats`; marcar estrela ★ com `@if (output.isUserOutput || output.address === activePoolUserAddressPart)`; em `handleSystemMessages()`, disparar `NO_MINING_REWARD` / `NOT_SOLO_MINING` apenas quando `info.coinUserIsAddress !== 0 && info.coinTicker !== 'SHA256'` (mantendo retrocompatibilidade se `coinUserIsAddress` estiver ausente em dispositivos legados). Atualizar tooltip do card Block Header para refletir a moeda ativa.
  - `PoolComponent`: adicionar dropdown de seleção de moeda (`0..10`) nos cartões de pool primária e fallback e revisar texto de aviso/tooltip BTC-only.
- **Alternativas consideradas**:
  - Criar um pipe separado (`coinAmount`): duplicaria lógica de formatação enquanto `SatsPipe` já centraliza toda renderização de valores coinbase.

## Risks / Trade-offs

- **[Risco] Moedas com bytes de versão idênticos aos do Bitcoin (`BCH`, `BSV`, `FB`, `BC2`) usarem endereços `1...`/`3...`/`bc1...`** → **Mitigação**: Em `Auto`, usar hints opcionais de `stratumURL` (e fallback seguro via comparação de payload binário), além do seletor manual explícito (`1..10`) em `PoolComponent` quando usuário quiser ticker/`block_time_s` exatos.
- **[Risco] `stratumUser` em pools PPS/FPPS ser apenas nome de usuário (não endereço)** → **Mitigação**: `coinUserIsAddress = 0` suprime `NO_MINING_REWARD` e `NOT_SOLO_MINING` sem precisar desligar `Decode Coinbase Tx`.
- **[Risco] Quebra de build Angular por campos novos obrigatórios no `SystemInfo`** → **Mitigação**: Rodar `npm run generate:api`, atualizar mocks em `system.service.ts` e validar com `npm run test:ci` na mesma etapa.

## Migration Plan

1. Implementar `coin_params` e alterações em `coinbase_decoder` (`components/stratum`), validar via `./tools/run_qemu_tests.sh`.
2. Conectar NVS, `GlobalState`, `stratum_v1_task.c` / `stratum_v2_task.c`, `system_api_json.c` e `openapi.yaml`; regenerar API Angular e atualizar mocks + UI (`SatsPipe`, `Home`, `Pool`) verificando `npm run test:ci` e `idf.py build`.
3. Realizar flash OTA mantendo `stratumCoinProfile = 0` (`Auto`) como padrão: em DGB (`lat-dgb.solofury.com`), confirmar imediatamente `coinTicker = "DGB"`, `coinBlockTimeSeconds = 15`, exibição de endereços `D...` / `dgb1...`, cálculo correto de `99%` de payout e ausência de banner `NO_MINING_REWARD` ou badges BIP falsos.
4. **Rollback**: Caso algum edge case apareça em uma pool específica, o usuário pode alternar `stratumCoinProfile` para `10` (`Generic SHA-256`) ou desmarcar `Decode Coinbase Tx` via AxeOS sem reflash.
