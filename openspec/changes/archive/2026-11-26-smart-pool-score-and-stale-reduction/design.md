# Design: Score Inteligente de Pool e Redução de Stale Shares (`smart-pool-score-and-stale-reduction`)

## Context

Veja [proposal.md](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/openspec/changes/smart-pool-score-and-stale-reduction/proposal.md).

Pontos verificados no código atual:
- [stratum_socket.c](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/components/stratum/stratum_socket.c#L148-L183) já aplica `TCP_NODELAY = 1` e `SO_KEEPALIVE = 1` (`keepidle = 60 s`), mas mantém `SO_RCVTIMEO = 180 s` fixo independentemente da moeda.
- [stratum_v1_task.c](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/main/tasks/stratum_v1_task.c#L365-L374) limpa `stratum_queue` quando `clean_jobs` chega e `count > 0`, mas não invalida jobs já despachados ao ASIC em `active_jobs`, fazendo [asic_result_task.c](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/main/tasks/asic_result_task.c#L52-L149) enviar shares obsoletos à pool após troca de bloco.
- [protocol_coordinator.c](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/main/tasks/protocol_coordinator.c) só aciona o fallback em erro fatal de conexão/setup (`COORD_EVENT_PROTOCOL_FAILED`), sem considerar RTT alto ou surtos de stales/rejeições.
- A Change 1 (`multi-coin-sha256-support`) já disponibilizou `coin_block_time_s` em `GlobalState` e `SystemInfo.coinBlockTimeSeconds` (`15 s` em DGB vs. `600 s` em BTC).

## Goals / Non-Goals

**Goals:**
- Evitar tráfego e penalizações na pool causadas por envio de shares expirados após `clean_jobs` (sem sacrificar contabilidade de hashrate local nem envio de possíveis blocos).
- Detectar conexões zumbis muito mais rápido em blockchains velozes (`<= 60 s`) sem causar reconexões falsas em Bitcoin (`600 s`).
- Quantificar a saúde da pool de forma justa por moeda (`0..100`) e habilitar troca inteligente para pool fallback apenas quando configurada explicitamente pelo usuário (`stratumSmartFailover`), sem *flapping*.

**Non-Goals:**
- Não alterar a estratégia de `stratum_v1` vs `stratum_v2` enquanto a conexão primária está saudável e o score está alto.
- Não suprimir shares cuja dificuldade encontrada atinja a dificuldade de rede do bloco (`nonce_diff >= network_nonce_diff`).

## Decisions

### 1. Módulo puro C `components/stratum/pool_score.{h,c}`
- **Escolha**:
  - Definir `PoolScoreState` contendo: `float rtt_ewma_ms`, `float stale_rate_ewma`, `float reject_rate_ewma`, `float disconnect_penalty`, `uint32_t sample_count`, `uint8_t degraded_streak`, `int64_t last_switch_us`, `uint32_t coin_block_time_s` e `uint8_t current_score`.
  - Funções puras (sem dependência de socket/FreeRTOS, testáveis em Unity/QEMU):
    - `pool_score_init(state, block_time_s)` e `pool_score_record_share(state, accepted, is_stale, rtt_ms)` (EWMA com $\alpha \approx 0.15$ em shares válidos/rejeitados e decaimento suave de `disconnect_penalty`).
    - `pool_score_record_disconnect(state)` (incrementa `disconnect_penalty`, limitado a teto seguro).
    - `pool_score_compute(state)`: normaliza a tolerância natural de *stales* pelo tempo de bloco:
      $$\text{budget\_stale\%} = \text{clamp}\left(\frac{100 \cdot (0.25 + \text{rtt\_s})}{\max(5.0, \text{block\_time\_s})},\ 0.25,\ 3.0\right)$$
      Assim, em DGB (`15 s`, `140 ms` RTT), `~1,0%` de *stales* permanece dentro da zona saudável (`score >= 80`), enquanto em BTC (`600 s`), `2,5%` de *stales* derruba fortemente o score. A pontuação final combina penalidade de RTT excedente, excesso de stale sobre o budget, rejeição não-stale e `disconnect_penalty`, limitada a `[0, 100]`.
    - `pool_score_should_failover(state, now_us, threshold, min_samples, min_streak, cooldown_us)`: retorna `true` somente quando `sample_count >= min_samples` (`10`), `degraded_streak >= min_streak` (`3`) e `(now_us - last_switch_us) >= cooldown_us` (`600 s`).
    - `pool_score_should_suppress_stale(job_epoch, current_epoch, nonce_diff, network_diff)`: retorna `true` se `job_epoch != current_epoch && (network_diff <= 0.0 || nonce_diff < network_diff)`.
- **Alternativas consideradas**:
  - Usar taxa simples acumulada de sessão (`shares_rejected / shares_accepted`): acumula erros de horas atrás e demora demais tanto para detectar degradação quanto para se recuperar após estabilização.

### 2. Rastreamento atômico de `job_epoch` e supressão local pós-`clean_jobs`
- **Escolha**:
  - Adicionar `uint32_t job_epoch` e `uint32_t active_job_epochs[128]` em `GlobalState` (protegidos pelo mutex `valid_jobs_lock` já existente em `create_jobs_task` e `asic_result_task`, evitando mexer na struct `bm_job`), além de `uint32_t stale_shares_suppressed` em `SystemModule`.
  - Incrementar `job_epoch` em `stratum_v1_task.c` sempre que chegar `MINING_NOTIFY` com `clean_jobs == true` (incondicionalmente, mesmo se `stratum_queue.count == 0`), e em `stratum_v2_task.c` quando mudar `prev_hash` ou houver job `clean`.
  - Em `create_jobs_task.c`, gravar `active_job_epochs[job_id] = job_epoch` ao registrar o slot sob `valid_jobs_lock`.
  - Em `asic_result_task.c`, após validar o `job_snapshot` sob `valid_jobs_lock` (capturando `snap_epoch` e `current_epoch`) e calcular `nonce_diff`, consultar `pool_score_should_suppress_stale(snap_epoch, current_epoch, nonce_diff, GLOBAL_STATE->network_nonce_diff)`:
    - Se `true`: incrementar `stale_shares_suppressed`, pular `STRATUM_V1_submit_share` / `stratum_v2_submit_share`, mas **continuar** chamando `SYSTEM_notify_found_nonce()` e `scoreboard_add()` para preservar o cálculo de `hashRate` e melhor dificuldade.
- **Alternativas consideradas**:
  - Apagar todos os `active_jobs[0..127]` no `clean_jobs`: causaria explosão de logs `"Invalid job nonce found"` e faria `asic_result_task` descartar nonces necessários para estimar o hashrate do ASIC durante a transição de bloco.

### 3. Timeouts Stratum sensíveis ao tempo de bloco + `protocol_coordinator` com `COORD_EVENT_POOL_DEGRADED`
- **Escolha**:
  - Em `components/stratum/stratum_socket.{h,c}`, criar `stratum_socket_set_options_for_coin(transport, uint32_t block_time_s)` (mantendo `stratum_socket_set_options(transport)` como wrapper padrão `600 s`). Quando `block_time_s > 0 && block_time_s <= 60`, usar `SO_RCVTIMEO = 60 s`, `TCP_KEEPIDLE = 20 s`, `TCP_KEEPINTVL = 5 s`, `TCP_KEEPCNT = 3`; caso contrário manter `180 s` / `60 s` / `10 s` / `3`.
  - Adicionar `COORD_EVENT_POOL_DEGRADED` e `protocol_coordinator_notify_degraded()` em `main/tasks/protocol_coordinator.{h,c}`.
  - Ao receber `COORD_EVENT_POOL_DEGRADED`, o coordenador só comuta para standby se:
    1. `stratumSmartFailover` estiver ativo em `SystemModule` / NVS;
    2. `!use_fallback_stratum` e `has_fallback_pool(gs)` verdadeiro;
    3. o cooldown (`600 s`) e os critérios de `pool_score_should_failover` estiverem satisfeitos;
    4. `probe_pool` na pool alternativa tiver sucesso.
    Nesse caso incrementa `smart_failover_count`, zera o histórico transitório de `PoolScoreState` da conexão antiga e comuta cleanly. Em standby (`COORD_STATE_RUNNING_FALLBACK`), o heartbeat continua testando a primária para *failback* após cooldown.
- **Alternativas consideradas**:
  - Failover automático ativado por padrão: arriscaria surpreender usuários que colocam uma pool backup pública apenas para emergências. Opt-in com default `false` preserva previsibilidade total.

### 4. NVS, OpenAPI e UI do AxeOS (`Pool`, `Home`, `Swarm`)
- **Escolha**:
  - Adicionar `NVS_CONFIG_STRATUM_SMART_FAILOVER` (`"smartfailover"`, `TYPE_BOOL`, default `false`) em `main/nvs_config.{h,c}`.
  - Expor em `system_api_json.c` e `openapi.yaml`: `stratumSmartFailover`, `poolHealthScore`, `poolRttEwmaMs`, `poolStaleRatePct`, `staleSharesSuppressed` e `smartFailoverCount`; regenerar modelos via `npm run generate:api` e atualizar `system.service.ts` mocks.
  - Em `PoolComponent`, adicionar o switch **Smart Pool Failover** com tooltip explicando histerese/cooldown.
  - Em `HomeComponent` (cartão Pool) e `SwarmComponent`, exibir badge de saúde (`0–100`) e contagem de stales suprimidos com guarda opcional (`?.` / `??`) para tolerar dispositivos legados no Swarm.
- **Alternativas consideradas**:
  - Apenas expor na API sem UI: dificultaria observar quando a supressão local evita shares perdidos ou ativar o recurso sem `curl`.

## Risks / Trade-offs

- **[Risco] Desconexões falsas caso uma pool rápida fique > 60 s sem enviar novo job em janela aleatória longa** → **Mitigação**: Mesmo em moedas rápidas (`15 s` em média), pools enviam `mining.notify` periódico de atualização de extranonce/ntime (ou `TCP_KEEPALIVE` sonda a cada `20 s`), garantindo atividade TCP antes de `60 s`.
- **[Risco] Falso positivo na supressão se `stratum_v1` receber dois `clean_jobs` muito próximos enquanto pipeline serial ainda drena resultado** → **Mitigação**: `network_nonce_diff` continua isento da supressão (candidatos a bloco sempre sobem) e a redução esperada em *stale rejects* compensa amplamente shares ordinários de jobs já substituídos.
- **[Risco] Quebra de contrato OpenAPI caso `system.service.ts` não acompanhe os novos campos** → **Mitigação**: Tarefas vinculam `npm run generate:api` + mocks + `npm run test:ci` em uma etapa fechada.

## Migration Plan

1. Implementar `pool_score.{h,c}` + `stratum_socket.{h,c}` + `test_pool_score.c` e validar no QEMU (`./tools/run_qemu_tests.sh`).
2. Integrar `job_epoch`, `asic_result_task.c`, `protocol_coordinator.c`, NVS e OpenAPI/AxeOS; validar `npm run test:ci` e `idf.py build`.
3. Realizar flash OTA na frota local mantendo `stratumSmartFailover = false` inicialmente:
   - Observar nas Bitaxes em `lat-dgb.solofury.com` se `stale_rejected` cai frente ao baseline atual de `0,96%–1,05%`, se `staleSharesSuppressed` incrementa após eventos de `clean_jobs` e se `poolHealthScore` reflete corretamente a latência (`~135–145 ms`) sem falsos alertas.
4. Opcionalmente habilitar `stratumSmartFailover = true` com uma pool de fallback configurada para validar troca controlada com histerese.
5. **Rollback**: Alternar `stratumSmartFailover` para `false` na UI do AxeOS (zero risco de failover inesperado); a supressão local não afeta blocos encontrados por preservar `nonce_diff >= network_nonce_diff`.
