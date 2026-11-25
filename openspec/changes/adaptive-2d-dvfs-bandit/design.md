# Design: DVFS 2D Adaptativo por Contextual Bandit (`dvfsProfile = 4: Adaptive`)

## Context

Veja [proposal.md](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/openspec/changes/adaptive-2d-dvfs-bandit/proposal.md) para motivação e escopo.

Estado atual relevante:
- `power_control_math.h` define os *Hard Caps* (`PCM_VCORE_MIN_MV = 1000`, `PCM_VCORE_MAX_MV = 1250`, `PCM_FREQ_MIN_MHZ = 400.0f`, `PCM_MAX_POWER_W = 32.0f`), `PcmDvfsProfile` (`0..3`) e `PcmRtcDiagnostics` (`PCM_RTC_DIAG_MAGIC = 0x53545244U`).
- `power_management_task.{h,c}` executa a malha de controle no Core 0 e gerencia `dvfs_profile`, `dvfs_freq_offset_mhz`, `dvfs_voltage_offset_mv`, `learned_max_freq_mhz`, `learned_min_vcore_mv` e `s_rtc_diag` (`RTC_NOINIT_ATTR`).
- A Fase 1 (`domain-cusum-pre-watchdog`) introduz `pre_watchdog_trigger` e `pre_watchdog_recoveries` em `PowerManagementModule` antes que ocorra travamento total e reset de watchdog.
- Os logs coletados nas 3 Bitaxes Gamma 601a mostram `~152 KB` de heap interno livre, `~7.5 MB` de SPIRAM livre e heterogeneidade entre placas (ex.: `.5` limitou anteriormente `learnedMaxFreq = 600` com `0.17%` de erro a `23.0 W`, enquanto `.22` opera em `20.8 W`).

## Goals / Non-Goals

**Goals:**
- Otimizar online o par `(frequência, VCore)` em malha 2D para maximizar hashrate efetivo útil ($H_{\text{eff}}$) respeitando estabilidade, temperatura e potência.
- Isolar toda a lógica matemática do Bandit em módulo C puro testável em Unity/QEMU (`components/asic/dvfs_bandit.{h,c}`).
- Garantir reversão imediata e determinística para um ponto seguro conhecido quando houver `pre_watchdog_trigger`, aumento abrupto de erros ou watchdog reset.
- Manter os perfis `0..3` intocados para viabilizar rollout A/B na frota.

**Non-Goals:**
- Não alterar a frequência de PLL do ASIC a cada segundo nem fazer saltos arbitrários na grade (apenas vizinhos de ±1 passo após janela mínima).
- Não substituir a máquina de estados térmica (`soft-throttling` / `overheat_mode`) nem a compensação de `Vdroop` (`10 s`) — ambas continuam soberanas acima do Bandit.
- Não persistir a tabela completa de estatísticas da grade em Flash NVS (apenas `adaptive_best_freq_mhz` e `adaptive_best_vcore_mv` em `RTC_NOINIT`).

## Decisions

### 1. Módulo `components/asic/dvfs_bandit.{h,c}` sobre grade discreta `15 × 21` com alocação em PSRAM
- **Escolha**: Definir grade discreta centrada na faixa útil de operação do BM1370:
  - Frequência: `500.0` a `675.0 MHz` em passos de `12.5 MHz` (`15` valores; clamping de segurança global permanece `400.0–675.0 MHz`).
  - VCore: `1050` a `1250 mV` em passos de `10 mV` (`21` valores; clamping global `1000–1250 mV`).
  - Cada célula da grade (`15 × 21 = 315` células) armazena contadores descontados (`weight_n`, `reward_sum`, `avg_heff`, `last_visited_step`, `blacklist_until_step` e contadores de falha).
  - Alocar o buffer da grade em PSRAM (`heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`) verificando `esp_psram_is_initialized()` no firmware, mantendo inicialização por ponteiro opaco em `dvfs_bandit.c` para permitir alocação estática/heap padrão nos testes Unity em QEMU caso PSRAM não esteja ativa.
- **Alternativas consideradas**:
  1. Regressão polinomial 2D contínua (RLS): menor footprint, porém mais propensa a extrapolação instável e passos não alinhados à granularidade física de PLL/PMBus.
  2. Tabela fixa menor sem VCore: não captura a fronteira ótima `(freq, VCore)` por lote de silício.

### 2. Discounted UCB com penalidades multi-objetivo, movimentação em vizinhança (`±1`), histerese e decaimento por drift
- **Escolha**:
  - Avaliação a cada `>= 60 s` fora de `throttle_active`, `overheat_mode` e cooldown de transição.
  - Função de recompensa normalizada por janela:
    $$R = H_{\text{eff,norm}} - \alpha \cdot \max(0, \text{err\%} - 0.5) - \beta \cdot \max(0, P - 28.0) - \gamma \cdot \max(0, T_{\text{chip}} - 62.0) - \delta \cdot \max(0, T_{\text{vr}} - 78.0) - \lambda_{\text{fault}}$$
    onde $H_{\text{eff}} = \text{hashrate\_ghs} \cdot (1 - \text{error\_percentage}/100)$ e os pesos $(\alpha, \beta, \gamma, \delta, \lambda_{\text{fault}})$ serão calibrados com os CSVs coletados por `scripts/collect_bitaxe_telemetry.py` antes da ativação em produção.
  - Índice Discounted UCB por célula candidata na vizinhança de Chebyshev/Manhattan limitada (`|Δf_idx| <= 1` e `|Δv_idx| <= 1`), sujeito às restrições preventivas térmicas e de potência (`<= 32.0 W`).
  - Histerese: mínimo de `N_dwell` avaliações consecutivas (`2` ciclos = `120 s`) no ponto atual antes de aceitar troca para célula vizinha, exceto em rollback de emergência.
  - Decaimento geométrico: multiplicação periódica (`γ_decay` horário / por janela) e boost de decaimento quando detectado drift de temperatura (`|ΔT|` significativo entre médias móveis) ou alta variação de erro, garantindo re-exploração quando a temperatura ambiente mudar entre dia e noite.
- **Alternativas consideradas**:
  1. `ε`-greedy global sobre todas as 315 células: saltos grandes de frequência/tensão causam spikes de corrente e drops de hashrate no BM1370.
  2. UCB padrão sem desconto: estatísticas antigas de madrugada impediriam adaptação rápida no pico térmico da tarde.

### 3. Hierarquia de segurança, integração com Fase 1 (`pre_watchdog_trigger`) e rollback
- **Escolha**:
  - Prioridade estrita na `power_management_task`:
    `Overheat / Hard Caps` > `Soft-Throttling Térmico` > `Watchdog / Pre-Watchdog Rollback` > `Vdroop (10 s)` > `DVFS Adaptive Bandit (60 s)`.
  - Quando `pre_watchdog_trigger` (Fase 1) ou `ASIC_watchdog` ocorrer durante `dvfs_profile == 4`:
    1. Registrar penalidade severa (`-λ_fault`) e bloqueio temporário (`cooldown`) na célula atual da grade.
    2. Reverter imediatamente os offsets RAM para `(adaptive_best_freq_mhz, adaptive_best_vcore_mv)` (ou, caso ainda não haja ponto seguro comprovado, recuar `-25 MHz` / manter NVS seguro).
    3. Iniciar janela de resfriamento/estabilização (`recovery_cooldown`) antes de retomar explorações positivas.
  - Atualizar `adaptive_best_freq_mhz` / `adaptive_best_vcore_mv` somente quando uma célula acumular múltiplas avaliações consecutivas estáveis sem falhas, CML/pre-watchdog ou throttling e superar o melhor $H_{\text{eff}}$ seguro anterior.

### 4. Evolução de `PcmRtcDiagnostics`, `nvs_config`, OpenAPI e AxeOS
- **Escolha**:
  - Adicionar `float adaptive_best_freq_mhz` e `uint16_t adaptive_best_vcore_mv` em `PcmRtcDiagnostics` e incrementar `PCM_RTC_DIAG_MAGIC` (`0x53545244U` -> `0x53545245U`) para evitar leitura inválida de CRC após OTA de versões antigas.
  - Expandir `NVS_CONFIG_DVFS_PROFILE` em `main/nvs_config.c` para `.min = 0, .max = 4` e definir `PCM_DVFS_PROFILE_ADAPTIVE = 4` em `power_control_math.h`.
  - Expor em `system_api_json.c` (`system_api_add_telemetry`) e `openapi.yaml`: `preWatchdogRecoveries`, `adaptiveBestFreq`, `adaptiveBestVcore` e `dvfsProfile` `0..4`.
  - Regenerar modelos TypeScript (`npm run generate:api`), atualizar mocks em `system.service.ts` e adaptar `EditComponent` (`Validators.max(4)` + opção `"Adaptive"`), `HomeComponent`, `SystemComponent` e `SwarmComponent`.

## Risks / Trade-offs

- **[Risco] Exploração agressiva elevar consumo ou erro antes de detectar instabilidade** → **Mitigação**: Restrição estrita a vizinhos `±1` (`12.5 MHz` / `10 mV`), pré-condições térmicas/potência antes de qualquer passo positivo, e integração com `pre_watchdog_trigger` da Fase 1 para recuo antecipado antes de travamento.
- **[Risco] Otimização local ("ótimo raso") após penalidade transitória de rede ou temperatura** → **Mitigação**: Decaimento geométrico periódico e expiração de blacklist da célula após janela configurável (`whitelist cooldown`).
- **[Risco] Incompatibilidade de struct `RTC_NOINIT` durante atualização OTA** → **Mitigação**: Bump explícito de `PCM_RTC_DIAG_MAGIC`, fazendo o firmware novo descartar com segurança dados RTC do formato antigo no primeiro boot.
- **[Risco] Quebra de contrato Angular/OpenAPI caso mocks fiquem defasados** → **Mitigação**: Tarefas obrigam `npm run generate:api` + atualização de `system.service.ts` + `npm run test:ci` em `axe-os`.

## Migration Plan

1. Pré-requisito: Concluir coleta baseline de 24h (`scripts/collect_bitaxe_telemetry.py`) e aplicar/validar a Fase 1 (`domain-cusum-pre-watchdog`).
2. Compilar e validar firmware com `dvfsProfile = 4` opt-in mantendo o default em `0` (e perfis `1..3` inalterados).
3. Realizar rollout A/B na frota local:
   - Manter `.21` no perfil atual (`Balanced`) como controle.
   - Ativar `dvfsProfile = 4` (`Adaptive`) em 1 unidade piloto (`.22` ou `.5`) e monitorar `hashRate`, `errorPercentage`, `preWatchdogRecoveries`, `watchdogResets`, `adaptiveBestFreq`/`Vcore` e `J/TH` por 48h via `scripts/collect_bitaxe_telemetry.py`.
4. **Rollback operacional**: Reverter imediatamente via UI do AxeOS (`Edit -> DVFS Profile`) ou `PATCH /api/system` com `{"dvfsProfile": 2}` (sem necessidade de refazer flash), o que zera os offsets do Bandit no ciclo seguinte da `power_management_task`.
