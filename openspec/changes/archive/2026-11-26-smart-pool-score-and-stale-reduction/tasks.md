# Tasks: Score Inteligente de Pool e Redução de Stale Shares (`smart-pool-score-and-stale-reduction`)

## 1. Módulo puro C de pontuação de pool e testes Unity (`components/stratum`)

- [x] 1.1 Criar `components/stratum/include/pool_score.h` e `components/stratum/pool_score.c` com `PoolScoreState`, EWMA de RTT/stale/reject, normalização por `coin_block_time_s`, penalidade decrescente de desconexões, `pool_score_should_failover()` (histerese + cooldown) e `pool_score_should_suppress_stale()`, registrando em `components/stratum/CMakeLists.txt` e validando compilação do componente `stratum`.
- [x] 1.2 Adicionar `stratum_socket_set_options_for_coin(transport, uint32_t block_time_s)` em `components/stratum/include/stratum_socket.h` e `components/stratum/stratum_socket.c` (`SO_RCVTIMEO = 60 s` / `TCP_KEEPIDLE = 20 s` quando `block_time_s <= 60`, preservando `180 s` / `60 s` para cadeias lentas e mantendo `stratum_socket_set_options()` como wrapper).
- [x] 1.3 Criar `components/stratum/test/test_pool_score.c` cobrindo clamping `0..100`, tolerância diferencial DGB (`15 s`) vs BTC (`600 s`), decaimento de desconexão, gates de histerese/cooldown de failover e supressão por `job_epoch` com exceção para candidatos a bloco, validando 100% de aprovação via `./tools/run_qemu_tests.sh`.

## 2. Supressão local de stales pós-`clean_jobs` e integração Stratum/Coordinator

- [x] 2.1 Adicionar `NVS_CONFIG_STRATUM_SMART_FAILOVER` (`"smartfailover"`, `TYPE_BOOL`, default `false`) em `main/nvs_config.{h,c}` e campos `job_epoch`, `active_job_epochs[128]`, `PoolScoreState`, `stale_shares_suppressed` e `smart_failover_count` em `main/global_state.h`.
- [x] 2.2 Atualizar `main/tasks/stratum_v1_task.c`, `main/tasks/stratum_v2_task.c`, `main/tasks/create_jobs_task.c` e `main/tasks/asic_result_task.c` para incrementar `job_epoch` em `clean_jobs` / novo `prev_hash`, carimbar `active_job_epochs[job_id]` sob `valid_jobs_lock`, aplicar timeouts sensíveis à moeda no socket, alimentar `PoolScoreState` e suprimir submissão Stratum de shares ordinários expirados preservando `SYSTEM_notify_found_nonce` e candidatos a bloco.
- [x] 2.3 Atualizar `main/tasks/protocol_coordinator.{h,c}` adicionando `COORD_EVENT_POOL_DEGRADED` e `protocol_coordinator_notify_degraded()` com checagens de opt-in (`stratumSmartFailover`), histerese, cooldown (`>= 600 s`) e `probe_pool` antes da troca, verificando compilação completa do firmware com `idf.py build`.

## 3. API REST, OpenAPI e mocks do AxeOS

- [x] 3.1 Expor `stratumSmartFailover`, `poolHealthScore`, `poolRttEwmaMs`, `poolStaleRatePct`, `staleSharesSuppressed` e `smartFailoverCount` em `main/http_server/system_api_json.c` (verificando contrato `GET /api/system/info` e `PATCH /api/system`).
- [x] 3.2 Atualizar `main/http_server/openapi.yaml` com os novos campos em `SystemInfo` e `Settings`, executar `npm run generate:api` em `main/http_server/axe-os` e atualizar os mocks em `main/http_server/axe-os/src/app/services/system.service.ts`, verificando compilação limpa via `npm run build`.

## 4. Interface AxeOS (`Pool`, `Home`, `Swarm`) e testes Jasmine

- [x] 4.1 Atualizar `PoolComponent` (`pool.component.{ts,html}`) com o toggle opt-in **Smart Pool Failover** (`stratumSmartFailover`) e `HomeComponent` / `SwarmComponent` para exibir `poolHealthScore`, taxa de stales e `staleSharesSuppressed` com guarda opcional para nós legados no Swarm.
- [x] 4.2 Ampliar `pool.component.spec.ts` e `home.component.spec.ts` verificando binding de `stratumSmartFailover` e renderização das métricas de saúde/stales da pool, validando 100% de aprovação com `npm run test:ci` em `main/http_server/axe-os`.

## 5. Verificação integrada e validação em hardware DGB

- [x] 5.1 Executar a bateria integrada final antes do deploy (`./tools/run_qemu_tests.sh`, `npm run test:ci` em `main/http_server/axe-os` e `idf.py build`), confirmando zero falhas ou regressões na suíte existente.
- [x] 5.2 Realizar flash OTA nas Bitaxes em `lat-dgb.solofury.com` (com `stratumSmartFailover = false` inicial), monitorar `/api/system/info` e telemetria para confirmar `poolHealthScore >= 80`, incremento de `staleSharesSuppressed` pós-`clean_jobs`, queda de `stale_rejected` frente ao baseline (`0,96%–1,05%`) e validar em seguida um teste controlado de failover com `stratumSmartFailover = true` e pool de backup ativa.

## Workflow follow-up

- Após validar os ganhos em campo na frota DGB, arquivar esta change via `/openspec-archive-change` (mesclando `smart-pool-scoring-and-stale-suppression`, `axeos-telemetry-and-dvfs-api` e `automated-test-coverage` para `openspec/specs/`).
- Com a saúde de pool e camada de comunicação estabilizadas, prosseguir para a Change 3 / Fase 3B (`predictive-thermal-control-rls-mpc`).
