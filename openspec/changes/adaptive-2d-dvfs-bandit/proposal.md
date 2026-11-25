# Proposal: DVFS 2D Adaptativo por Contextual Bandit (`dvfsProfile = 4: Adaptive`)

## Why

Os perfis DVFS atuais (`1=Efficiency`, `2=Balanced`, `3=Performance`) ajustam frequência e tensão em eixos reativos e limites fixos, sem otimizar conjuntamente o ponto `(frequência, tensão)` que maximiza o hashrate efetivo útil ($H_{\text{eff}}$) diante de variações térmicas e do processo individual de silício de cada Bitaxe Gamma 601a (BM1370). Agora que a base I2C/WS/coredump está estabilizada e a Fase 1 (`domain-cusum-pre-watchdog`) introduz sinais precoces de anomalia antes de resets, é possível adicionar o perfil opt-in `4 (Adaptive)` para aprender online o ponto ótimo de operação com rollback automático para um ponto seguro conhecido.

## What Changes

- Adicionar o perfil DVFS opt-in `4` (`PCM_DVFS_PROFILE_ADAPTIVE` / `Adaptive`) ao firmware, preservando integralmente os perfis `0` (`Manual`), `1` (`Efficiency`), `2` (`Balanced`) e `3` (`Performance`) para possibilitar rollout A/B entre placas da frota.
- Introduzir um controlador Contextual Bandit / Multi-Armed Bandit 2D em C puro (`components/asic/dvfs_bandit.{h,c}`) operando sobre grade discreta `(frequência, tensão)` em passos seguros de `12.5 MHz` e `10 mV`, usando Discounted UCB com penalidades por erro ASIC, consumo, temperatura, eventos de pré-watchdog e resets de watchdog.
- Integrar o ciclo de aprendizado ao `power_management_task` com histerese mínima de permanência, detecção de drift ambiental/térmico por janela e preservação do último ponto seguro (`adaptive_best_freq_mhz`, `adaptive_best_vcore_mv`) em `PcmRtcDiagnostics` entre warm resets.
- Estender a API REST/OpenAPI (`dvfsProfile` `0..4` e campos read-only `adaptiveBestFreq`, `adaptiveBestVcore` e `preWatchdogRecoveries` em `SystemInfo`), o `nvs_config` e os componentes do AxeOS (`Edit`, `Home`, `System`, `Swarm`), mantendo os dados mockados em `system.service.ts` sincronizados.
- Expandir a suíte de testes unitários C (Unity/QEMU) para o algoritmo 2D Bandit e persistência RTC, além dos testes Jasmine do AxeOS para o novo perfil e telemetria.

## Capabilities

### New Capabilities

_(Nenhuma — os novos comportamentos evoluem diretamente os contratos existentes de DVFS, API/AxeOS e cobertura de testes.)_

### Modified Capabilities

- `vdroop-and-dvfs-autotuner`: Adição do perfil opt-in `4` (`Adaptive`) com controlador Contextual Bandit 2D por $H_{\text{eff}}$, histerese de passo, decaimento temporal/drift, integração com pré-watchdog/watchdog, rollback para último ponto seguro e persistência em `PcmRtcDiagnostics`.
- `axeos-telemetry-and-dvfs-api`: Ampliação da faixa de `dvfsProfile` de `0..3` para `0..4` em NVS, OpenAPI e UI do AxeOS, além da exposição dos novos campos de telemetria (`adaptiveBestFreq`, `adaptiveBestVcore`, `preWatchdogRecoveries`) em `/api/system/info`.
- `automated-test-coverage`: Inclusão de casos de teste unitários em C para o módulo `dvfs_bandit` e atualização dos testes de diagnósticos RTC e specs Jasmine do AxeOS.

## Impact

- **Firmware C (`components/asic/`, `main/tasks/`, `main/`)**: `dvfs_bandit.{h,c}`, `power_control_math.h`, `power_management_task.{h,c}`, `nvs_config.c`, `system_api_json.c` e `components/asic/CMakeLists.txt`. Alocação da matriz de estatísticas da grade em PSRAM (`esp_psram_is_initialized()`).
- **API & Frontend (`main/http_server/`)**: `openapi.yaml` (regeneração via `npm run generate:api`), `system.service.ts`, `edit.component.{ts,html,spec.ts}`, `home.component.{ts,spec.ts}`, `system.component.{ts,spec.ts}` e `swarm.component.ts`.
- **Testes**: `components/asic/test/test_dvfs_bandit.c`, `components/asic/test/test_power_control.c` e specs Jasmine do AxeOS.
- **Dependência lógica**: Pressupõe a implementação prévia da Fase 1 (`domain-cusum-pre-watchdog`) para o fornecimento dos eventos/contadores de pré-watchdog (`pre_watchdog_trigger`, `pre_watchdog_recoveries`).
