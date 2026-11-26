# Proposal: Score Inteligente de Pool e Redução de Stale Shares (`smart-pool-score-and-stale-reduction`)

## Why

A telemetria em campo em `lat-dgb.solofury.com` ([20261126-085811](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/backups/telemetry/20261126-085811/telemetry.csv)) registrou ~`0,96%–1,05%` de *stale shares* com RTT mediano de `135–145 ms` (`p95 ~154 ms`). No código atual, `asic_result_task` continua enviando à rede shares gerados antes de um `clean_jobs`, `stratum_socket` usa `SO_RCVTIMEO = 180 s` fixo (longo demais para blocos de `15 s` em DGB), e o `protocol_coordinator` só realiza *failover* após queda total da conexão. Agora que a Change 1 (`multi-coin-sha256-support`) expõe `coin_block_time_s`, é possível normalizar a saúde da pool pelo tempo de bloco e suprimir trabalho obsoleto antes da transmissão Stratum.

## What Changes

- **Motor puro C de Score da Pool (`pool_score`)**: pontuação rolling (`0..100`) baseada em EWMA de RTT, taxa rolling de *stales* normalizada por `coin_block_time_s`, rejeições não-stale e penalidades com decaimento por desconexão.
- **Supressão local de *stale shares* por geração (`job_epoch`)**: incremento atômico de `job_epoch` em `clean_jobs` (Stratum v1) ou novo `prev_hash` (Stratum v2), suprimindo envio à rede de shares comuns de épocas expiradas sem afetar a contagem local de hashrate e mantendo o envio integral caso o share atinja dificuldade de bloco (`nonce_diff >= network_nonce_diff`).
- **Timeouts Stratum adaptados à moeda**: ajuste de `SO_RCVTIMEO` e `TCP_KEEPIDLE`/`TCP_KEEPINTVL` conforme `coin_block_time_s` (`60 s` / `20 s` para blocos rápidos `<= 60 s` como DGB, preservando `180 s` / `60 s` para cadeias lentas como BTC), mantendo `TCP_NODELAY = 1`.
- **Failover/Failback inteligente opt-in com histerese**: configuração `stratumSmartFailover` (default `false`) no `protocol_coordinator`, acionando troca para pool de backup apenas após degradação sustentada (`score < 45`), cooldown mínimo (`>= 600 s`) e verificação prévia de alcançabilidade, além de retorno periódico quando a primária se recuperar.
- **API, AxeOS e Testes**: exposição de `poolHealthScore`, `poolRttEwmaMs`, `poolStaleRatePct`, `staleSharesSuppressed`, `smartFailoverCount` e `stratumSmartFailover` em `/api/system/info` e OpenAPI, toggle em `PoolComponent`, indicadores em `HomeComponent` / `SwarmComponent`, testes Unity (`test_pool_score.c`) e specs Jasmine.

## Capabilities

### New Capabilities

- `smart-pool-scoring-and-stale-suppression`: Motor de pontuação rolling de saúde da pool (`0–100`) normalizado pelo `coin_block_time_s`, ajuste de `SO_RCVTIMEO`/`TCP_KEEPALIVE` sensível ao tempo de bloco, supressão local de shares obsoletos pós-`clean_jobs` (preservando candidatos a bloco e contagem de hashrate) e failover/failback inteligente opt-in com histerese e cooldown.

### Modified Capabilities

- `axeos-telemetry-and-dvfs-api`: Configuração opt-in `stratumSmartFailover` em NVS/PATCH/OpenAPI, exposição de `poolHealthScore`, `poolRttEwmaMs`, `poolStaleRatePct`, `staleSharesSuppressed` e `smartFailoverCount` em `GET /api/system/info`, sincronização de mocks em `system.service.ts` e exibição na UI do AxeOS (`Pool`, `Home`, `Swarm`).
- `automated-test-coverage`: Testes Unity em `components/stratum/test/test_pool_score.c` cobrindo pontuação por RTT/stale/reject/desconexão, normalização por `block_time_s` (DGB `15 s` vs BTC `600 s`), histerese/cooldown de failover e lógica de supressão por `job_epoch`, mais specs Jasmine em `pool.component.spec.ts` e `home.component.spec.ts`.

## Impact

- **Stratum & Socket C (`components/stratum/`)**: novo `pool_score.{h,c}`, atualização de `stratum_socket.{h,c}`, `CMakeLists.txt` e novo `test/test_pool_score.c`.
- **NVS, Estado & Tasks (`main/`)**: `nvs_config.{h,c}`, `global_state.h`, `tasks/stratum_v1_task.c`, `tasks/stratum_v2_task.c`, `tasks/create_jobs_task.c`, `tasks/asic_result_task.c` e `tasks/protocol_coordinator.{h,c}`.
- **HTTP/OpenAPI & AxeOS (`main/http_server/`)**: `system_api_json.c`, `openapi.yaml` (`npm run generate:api`), `system.service.ts` mocks, `pool.component.{ts,html,spec.ts}`, `home.component.{ts,html,spec.ts}` e `swarm.component.{ts,html}`.
