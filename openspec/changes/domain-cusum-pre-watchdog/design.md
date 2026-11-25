# Design

## Context

Veja `proposal.md` para a motivação. Os seguintes invariantes do firmware moldam o desenho:

- **Monitoramento no Core 1 (`main/tasks/hashrate_monitor_task.c`)**:
  - Executa a `1 Hz` (`POLL_RATE = 1000 ms`), lê `domain_measurements[asic][0..hash_domains-1]` sob `HASHRATE_MONITOR_MODULE->lock` e já calcula `watchdog_armed` com carência de `30 s` após inicialização ou mudança de frequência (`last_freq_change_us`), além das guardas `!pools_unavailable`, `!mining_paused`, `overheat_mode == 0`, `power_fault == 0`, `!hardware_fault`, `!SELF_TEST_MODULE.is_active` e `!asic_watchdog_trigger`.
  - Leituras ao vivo nas 3 Bitaxe Gamma 601a (`BM1370`, 4 domínios) mostram variação instantânea de 1 s até `r ≈ 0,79` mesmo com `0%` de erro (ex.: `[232, 292, 279, 311] GH/s`), exigindo filtragem temporal antes de acumular anomalia.
  - Um dataset de 24h está sendo coletado em paralelo via `scripts/collect_bitaxe_telemetry.py` para calibrar empiricamente os percentis de `dom_min_ratio` (`--analyze`).
- **Regulação de energia e DVFS no Core 0 (`main/tasks/power_management_task.c`, `components/asic/power_control_math.{h,c}`)**:
  - Roda em ciclos de `100 ms` (`handle_thermal_state_machine`, `handle_dvfs_autotuner`, `handle_vdroop_compensation`, `apply_effective_power_and_freq`) respeitando os *Hard Caps* (`1000–1250 mV`, `400–675 MHz`, `32.0 W`) e mantendo o watchdog de 3 níveis (`handle_asic_watchdog`).
  - A Fase 2 (`adaptive-2d-dvfs-bandit`) foi definida para introduzir um novo **`dvfsProfile = 4` (`Adaptive / Smart`)**, mantendo os perfis `0..3` intactos e consumindo os sinais de instabilidade do Pré-Watchdog.

## Goals / Non-Goals

**Goals:**
- Isolar toda a matemática do filtro `EWMA + CUSUM` e da decisão de alívio preventivo em funções C puras em `components/asic/power_control_math.{h,c}`, 100% testáveis em QEMU via Unity.
- Aplicar alívio a quente em RAM (`+5 mV` ou `-12,5 MHz`) sem invocar `mining_stop()`, preservando zero *downtime* em degradações incipientes.
- Evitar contenção adicional de mutex no Core 1 e fornecer campos de contrato limpos em `PowerManagementModule` prontos para o Bandit 2D da Fase 2 (`dvfsProfile = 4`).

**Non-Goals:**
- Implementar o `dvfsProfile = 4` (`Adaptive / Smart`) nesta change (pertence à change separada `adaptive-2d-dvfs-bandit`).
- Alterar `openapi.yaml`, `nvs_config`, `PcmRtcDiagnostics` ou a UI do AxeOS nesta fase.
- Substituir, desarmar ou relaxar os critérios do watchdog de 3 níveis (`0 GH/s` por `60 s` ou domínio `< 10%` por `45 s`).

## Decisions

### Decision 1: Filtro `EWMA + CUSUM` unilateral por domínio em `power_control_math.{h,c}`
- **Escolha**:
  - Definir `PCM_CUSUM_MAX_DOMAINS = 4`, `PCM_CUSUM_ALPHA = 0.20f`, `PCM_CUSUM_RATIO_MAX = 1.50f`, `PCM_CUSUM_DEFAULT_DRIFT_K = 0.78f` e `PCM_CUSUM_DEFAULT_THRESHOLD_H = 2.0f` (ajustáveis após inspeção do `--analyze` de 24h antes do rollout).
  - Estrutura `PcmDomainCusumState` (`ewma_ratio[4]`, `cusum[4]`) inicializada com `ewma_ratio[d] = 1.0f` e `cusum[d] = 0.0f` via `pcm_domain_cusum_reset()`.
  - Em `pcm_domain_cusum_update(state, domain_hr, domain_count, k, h, &out_domain, &out_cusum)`:
    - Se `domain_count <= 1`, retorna `false`.
    - Para cada domínio `d`: calcula `other_avg = (sum - domain_hr[d]) / (domain_count - 1)`. Se `other_avg <= 0.0001f`, ignora o ciclo para evitar divisão por zero.
    - Calcula `ratio = clamp(domain_hr[d] / other_avg, 0.0f, 1.50f)`.
    - Atualiza `ewma = 0.20f * ratio + 0.80f * ewma` e `s = fmaxf(0.0f, s + (k - ewma))`.
    - Retorna `true` com o domínio de maior `cusum` quando `max_s >= h`.
- **Rationale e Alternativas**:
  - *Limiar instantâneo ou média móvel simples*: falha com o ruído de 1 s observado no BM1370 (`0.79`) ou demora demais para acumular déficit pequeno e contínuo (`0.55`). O `EWMA + CUSUM` ocupa apenas `32 B` por ASIC, roda em $O(D)$ sem alocação dinâmica e reage proporcionalmente à severidade da queda (~`8–15 s`).

### Decision 2: Contrato de sinalização Core 1 -> Core 0 em `PowerManagementModule`
- **Escolha**:
  - Adicionar em `PowerManagementModule` (`main/tasks/power_management_task.h`):
    - `volatile bool pre_watchdog_trigger`
    - `uint8_t pre_watchdog_asic`
    - `uint8_t pre_watchdog_domain`
    - `float pre_watchdog_cusum_max`
    - `uint32_t pre_watchdog_recoveries`
    - `int64_t last_pre_watchdog_action_us`
  - No Core 1 (`hashrate_monitor_task.c`), copiar apenas os floats `domain_measurements[asic_nr][0..hash_domains-1]` para um buffer local na stack enquanto retém `HASHRATE_MONITOR_MODULE->lock` (junto às somas já lidas), liberando o mutex antes de rodar `pcm_domain_cusum_update()`.
  - Quando `!watchdog_armed` (ou quando `pre_watchdog_trigger` ainda está pendente, ou após disparar), chamar `pcm_domain_cusum_reset()` para todos os ASICs monitorados.
- **Alternativas consideradas**:
  - *Criar fila FreeRTOS separada*: desnecessário para eventos raros de 1 bit + 2 metadados; o padrão por `volatile bool` + campos de diagnóstico já é o usado por `asic_watchdog_trigger` neste repositório.

### Decision 3: Alívio a quente consciente de perfil (`pcm_pre_watchdog_apply_relief`) e *cooldown* de `60 s`
- **Escolha**:
  - Em `power_management_task.c`, executar `handle_pre_watchdog(GLOBAL_STATE)` logo após `handle_dvfs_autotuner()` e `handle_vdroop_compensation()`, **antes** de `apply_effective_power_and_freq()`, garantindo a ordem segura (sobe VCore antes de freq / reduz freq antes de VCore).
  - Regras em `pcm_pre_watchdog_apply_relief()`:
    - Guarda de *cooldown*: se `(now_us - last_pre_watchdog_action_us) < 60 s` (quando `> 0`), limpa `pre_watchdog_trigger` sem alterar `Freq`/`VCore`.
    - **Alívio por tensão (`+5 mV`)**: permitido somente quando o perfil permite escalar tensão (`Efficiency` ou `Performance`, mantendo preparado para o perfil `4` na Fase 2), `power_w < 30.5f`, `vr_temp_c < 84.0f` e `effective_vcore_mv <= 1245` (`PCM_VCORE_MAX_MV - 5`). Incrementa `dvfs_voltage_offset_mv` (limitado ao teto `1250 mV` em `pcm_compute_effective_vcore`).
    - **Alívio por frequência (`-12.5 MHz`)**: aplicado em `Balanced`, `Manual` ou quando faltar margem de tensão/potência/VR. Reduz `dvfs_freq_offset_mhz` (ou `watchdog_freq_offset_mhz` em `Manual`/piso de perfil) respeitando o piso `400.0 MHz`, e trava `learned_max_freq_mhz` em `Balanced`/`Performance` (chamando `sync_rtc_diagnostics()` quando alterado).
    - Em qualquer alívio aplicado: incrementa `pre_watchdog_recoveries`, atualiza `last_pre_watchdog_action_us = now_us` e define `last_dvfs_step_us = now_us` para impedir que o auto-tuner suba novamente no segundo seguinte.
- **Rationale**:
  - Em `Balanced` (`dvfsProfile = 2`, usado hoje nas 3 Bitaxes), o contrato atual fixa `dvfs_voltage_offset_mv = 0`, portanto o alívio seguro e coerente é `-12.5 MHz` em RAM sem reiniciar o ASIC. Já em `Efficiency`/`Performance` (e futuramente no `dvfsProfile = 4`), `+5 mV` preserva o hashrate quando há folga térmica e elétrica.

### Decision 4: Contrato de integração para a Fase 2 (`adaptive-2d-dvfs-bandit`, `dvfsProfile = 4`)
- **Escolha**:
  - Na Fase 2, o Bandit 2D no Core 0 inspecionará `pre_watchdog_trigger` / incremento de `pre_watchdog_recoveries` no início do ciclo de `POWER_MANAGEMENT_task` para aplicar penalidade imediata na célula `(Freq, VCore)` ativa antes de encerrar a janela de `60 s`.

## Risks / Trade-offs

- **Falso positivo em chip com leve assimetria permanente de domínio** -> Mitigado pelo `EWMA` (`alpha = 0.20`), pelo piso `k = 0.78` calibrado com os percentis reais (`p01`) de 24h do `scripts/collect_bitaxe_telemetry.py --analyze`, e pelo *cooldown* de `60 s`.
- **Alívio por `+5 mV` não reseta `last_freq_change_us`** -> Sem cuidado extra, o `cusum` poderia permanecer alto no segundo seguinte. Mitigado zerando `PcmDomainCusumState` imediatamente no disparo e aplicando o *cooldown* de `60 s` no Core 0.
- **Placas multi-ASIC (`asic_count > 1`)** -> `hashrate_monitor_task.c` alocará o vetor `PcmDomainCusumState[asic_count]` usando os mesmos caps (`MALLOC_CAP_SPIRAM` com fallback) já adotados pelos buffers do módulo, ignorando ASICs com `hash_domains <= 1`.
