# Tasks

## 1. Persistência `RTC_NOINIT` e Diagnóstico de Watchdog (`components/asic` e `main/tasks`)

- [ ] 1.1 Definir `PcmRtcDiagnostics`, funções puras `pcm_rtc_diag_seal()` / `pcm_rtc_diag_validate()` (magic + fingerprint NVS + CRC32) em `components/asic/include/power_control_math.h` e `components/asic/power_control_math.c`, adicionando casos `TEST_CASE` em `components/asic/test/test_power_control.c` validando restauração após warm reset, descarte em `ESP_RST_POWERON`, detecção de CRC corrompido e invalidação por mudança NVS; verificar com `./tools/run_qemu_tests.sh` (ou build em `test-ci/`) com 0 falhas.
- [ ] 1.2 Adicionar `watchdog_trigger_reason` e `last_watchdog_reason` a `PowerManagementModule` (`main/tasks/power_management_task.h`), preencher a causa em `hashrate_monitor_task.c` (`1 = Zero Hashrate`, `2 = Frozen Domain`), e integrar o bloco `static RTC_NOINIT_ATTR PcmRtcDiagnostics s_rtc_diag;` em `main/tasks/power_management_task.c` (restaurando no boot quando `esp_reset_reason() != ESP_RST_POWERON` e selando em atualizações de `watchdog_resets`, `cml_error_count`, limites DVFS e antes de `esp_restart()` no Nível 3).

## 2. Contrato HTTP/OpenAPI e Mocks (`main/http_server`)

- [ ] 2.1 Expor `learnedMaxFreq`, `learnedMinVcore` e `lastWatchdogReason` em `system_api_add_telemetry()` (`main/http_server/system_api_json.c`) e declará-los em `SystemInfo` de `main/http_server/openapi.yaml`.
- [ ] 2.2 Executar `npm run generate:api` em `main/http_server/axe-os`, atualizar o objeto mockado em `main/http_server/axe-os/src/app/services/system.service.ts` e confirmar compilação TypeScript limpa com `npm run build` / `npm run test:ci`.

## 3. Painel Comparativo de Frota no `SwarmComponent` (`AxeOS`)

- [ ] 3.1 Atualizar `main/http_server/axe-os/src/app/components/swarm/swarm.component.ts` e `swarm.component.html` para calcular e exibir `J/TH` da frota e por dispositivo, badge do perfil DVFS (`Manual`, `Efficiency`, `Balanced`, `Performance`), clock/VCore efetivos (`+vdroopOffsetMv` e tooltip de limite aprendido) e notificações em `getDeviceNotification()` para `throttleActive === 1`, `watchdogResets > 0` e `(targetFrequency ?? frequency) < 400`.
- [ ] 3.2 Expandir `main/http_server/axe-os/src/app/components/swarm/swarm.component.spec.ts` com testes unitários para cálculo de `J/TH`, compatibilidade com campos ausentes de placas legadas e regras de `getDeviceNotification()`, executando `npm run test:ci` em `main/http_server/axe-os` com 0 falhas.

## 4. Verificação Integrada

- [ ] 4.1 Executar `idf.py build` na raiz do projeto, `idf.py build` em `test-ci/` e `npm run test:ci` em `main/http_server/axe-os`, confirmando build completo de firmware + Web UI sem erros.
