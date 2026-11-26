# Tasks: DVFS 2D Adaptativo por Contextual Bandit (`dvfsProfile = 4: Adaptive`)

## 1. Módulo Puro C do Bandit 2D e Cobertura Unity

- [x] 1.1 Criar `components/asic/include/dvfs_bandit.h` e `components/asic/dvfs_bandit.c` com a grade `15 × 21` (`500.0–675.0 MHz` em passos de `12.5 MHz`, `1050–1250 mV` em passos de `10 mV`, clamping rígido `400.0–675.0 MHz` e `1000–1250 mV`), cálculo de $H_{\text{eff}}$ e recompensa descontada, seleção Discounted UCB na vizinhança `±1`, histerese de permanência, decaimento temporal/por drift e rollback com penalização de célula; registrar em `components/asic/CMakeLists.txt` e validar compilação via build dos testes `components/asic`.
- [x] 1.2 Adicionar `PCM_DVFS_PROFILE_ADAPTIVE = 4` em `components/asic/include/power_control_math.h` e estender `PcmRtcDiagnostics` com `adaptive_best_freq_mhz` e `adaptive_best_vcore_mv` (incrementando `PCM_RTC_DIAG_MAGIC` para `0x53545245U`), verificando compilação e compatibilidade em `power_control_math`.
- [x] 1.3 Criar `components/asic/test/test_dvfs_bandit.c` e atualizar os testes RTC em `components/asic/test/test_power_control.c`, cobrindo indexação, hard caps, histerese, decaimento por drift, rollback/penalização em falha e round-trip CRC32 de `PcmRtcDiagnostics`, verificando aprovação via `./tools/run_qemu_tests.sh`.

## 2. Integração no Firmware (`nvs_config` e `power_management_task`)

- [x] 2.1 Atualizar `NVS_CONFIG_DVFS_PROFILE` em `main/nvs_config.c` de `.max = 3` para `.max = 4` e verificar validação de NVS no código de settings em `main/http_server/http_server.c`.
- [x] 2.2 Alocar e gerenciar o estado do Bandit em `main/tasks/power_management_task.{h,c}` usando PSRAM (`esp_psram_is_initialized()` com `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`), integrando `PCM_DVFS_PROFILE_ADAPTIVE` no loop de `>= 60 s` abaixo da camada térmica/Vdroop e conectando `pre_watchdog_trigger` (Fase 1) e `ASIC_watchdog` ao rollback imediato + penalização de célula.
- [x] 2.3 Atualizar `sync_rtc_diagnostics()` e a restauração pós-boot em `main/tasks/power_management_task.c` para salvar/restaurar `adaptive_best_freq_mhz` e `adaptive_best_vcore_mv` (zerando em mudanças de `target_frequency`, `target_voltage` ou `dvfs_profile`) e verificar compilação completa do firmware com `idf.py build`.

## 3. API REST, OpenAPI e Mocks do AxeOS

- [x] 3.1 Expor `preWatchdogRecoveries`, `adaptiveBestFreq` e `adaptiveBestVcore` em `main/http_server/system_api_json.c` (`system_api_add_telemetry`) e validar o payload JSON de `GET /api/system/info` e WebSocket `system_api_get_full_json`.
- [x] 3.2 Atualizar `main/http_server/openapi.yaml` (`SystemInfo.dvfsProfile` e `Settings.dvfsProfile` para `minimum: 0`, `maximum: 4`, além das propriedades read-only `preWatchdogRecoveries`, `adaptiveBestFreq` e `adaptiveBestVcore`), executar `npm run generate:api` em `main/http_server/axe-os` e verificar geração dos modelos em `src/app/generated/`.
- [x] 3.3 Atualizar os dados mockados em `main/http_server/axe-os/src/app/services/system.service.ts` para incluir os novos campos de `SystemInfo` e verificar compilação limpa com `npm run build` em `main/http_server/axe-os`.

## 4. Interface AxeOS e Testes Unitários Angular

- [x] 4.1 Atualizar `main/http_server/axe-os/src/app/components/edit/edit.component.ts` (adicionar opção `Adaptive` com valor `4` em `dvfsProfileOptions` e ajustar `Validators.max(4)`) e verificar envio de `{"dvfsProfile": 4}` sem exigir restart em `edit.component.spec.ts`.
- [x] 4.2 Atualizar `HomeComponent`, `SystemComponent` e `SwarmComponent` (`main/http_server/axe-os/src/app/components/{home,system,swarm}/`) para formatar `dvfsProfile = 4` como `Adaptive` e exibir `preWatchdogRecoveries`, `adaptiveBestFreq` e `adaptiveBestVcore` quando disponíveis.
- [x] 4.3 Atualizar e ampliar os specs Jasmine (`edit.component.spec.ts`, `home.component.spec.ts`, `system.component.spec.ts`) e verificar 100% de aprovação executando `npm run test:ci` em `main/http_server/axe-os`.

## 5. Verificação Integrada e Rollout Piloto A/B

- [x] 5.1 Executar a bateria integrada final antes do deploy (`./tools/run_qemu_tests.sh`, `npm run test:ci` em `main/http_server/axe-os` e `idf.py build`) e confirmar zero falhas de compilação ou regressão nos perfis `0..3`.
- [x] 5.2 Realizar flash OTA (`www.bin` + `esp-miner.bin`) nas 3 Bitaxes (`192.168.1.101`, `192.168.1.102`, `192.168.1.103`) e rollout piloto A/B: ativar `dvfsProfile = 4` (`Adaptive`) em `192.168.1.101` mantendo `192.168.1.102` e `192.168.1.103` como controle (`Balanced` = `2`), verificando `verify-post-flash` e telemetria coletada (`scripts/collect_bitaxe_telemetry.py`) sem regressão de resets/erros.

## Workflow follow-up

- Pré-requisito obrigatório antes de implementar as tarefas acima: aplicar e validar a change `domain-cusum-pre-watchdog` (Fase 1), que fornece `pre_watchdog_trigger` e `pre_watchdog_recoveries` consumidos pela integração de rollback do Bandit.
- Após concluir a implementação e validar o rollout A/B de 48h na frota, arquivar esta change via `/openspec-archive-change` (que mesclará os deltas de `vdroop-and-dvfs-autotuner`, `axeos-telemetry-and-dvfs-api` e `automated-test-coverage` para `openspec/specs/`).
