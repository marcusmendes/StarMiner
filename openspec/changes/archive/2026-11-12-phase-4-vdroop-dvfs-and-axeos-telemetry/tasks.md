# Tasks

## 1. Persistência NVS de `dvfsProfile` e Estrutura `PowerManagementModule`

- [x] 1.1 Adicionar `NVS_CONFIG_DVFS_PROFILE` em `main/nvs_config.h` e registrá-lo em `settings[]` em `main/nvs_config.c` (`nvs_key_name = "dvfsprofile"`, `type = TYPE_U16`, `default_value = {.u16 = 0}`, `rest_name = "dvfsProfile"`, `min = 0`, `max = 3`), verificando que `PATCH /api/system` aceita e valida `dvfsProfile` via `check_settings_and_update()`.
- [x] 1.2 Adicionar em `main/tasks/power_management_task.h` (struct `PowerManagementModule`) os campos de *Vdroop* e Auto-Tuner DVFS (`uint16_t vdroop_offset_mv`, `int64_t last_vdroop_check_us`, `uint16_t dvfs_profile`, `float dvfs_freq_offset_mhz`, `int16_t dvfs_voltage_offset_mv`, `int64_t last_dvfs_step_us`), verificando a presença de todos os campos no header.

## 2. Compensação de *Vdroop* em Malha Lenta e Perfis DVFS (`power_management_task.c`)

- [x] 2.1 Implementar em `main/tasks/power_management_task.c` a compensação de *Vdroop* em malha lenta (`10 s`): quando `ASIC_initalized`, `!throttle_active`, `!overheat_mode` e após `10 s` da última mudança de frequência, comparar `core_voltage` com a tensão base pretendida (`base_voltage_mv`) e ajustar `vdroop_offset_mv` em passos de `+5 mV` (se `core_voltage < base_voltage_mv - 5.0f` e `power <= 32.0 W`) até o teto de `+20 mV` ou `-5 mV` (se `core_voltage > base_voltage_mv + 5.0f`) até `0 mV`, respeitando `1250 mV` e zerando `vdroop_offset_mv` em `mining_stop()`, verificando a lógica no código.
- [x] 2.2 Implementar em `main/tasks/power_management_task.c` a máquina de estados dos 4 perfis DVFS (`0 = Manual`, `1 = Eficiência` `[500–600 MHz, erro <= 0.5%]`, `2 = Equilibrado` `[525–625 MHz, erro <= 0.8%, ASIC < 64 °C, VR < 80 °C]`, `3 = Performance` `[550–675 MHz, erro <= 1.0%, ASIC < 68 °C, VR < 90 °C, power < 31 W]`) avaliada a cada `60 s` fora de `throttle_active` e `overheat_mode`, integrando `dvfs_freq_offset_mhz`, `dvfs_voltage_offset_mv` e `vdroop_offset_mv` ao cálculo de `effective_freq_mhz` e `effective_voltage_mv` dentro dos *Hard Caps* (`1000–1250 mV`, `400–675 MHz`, `32.0 W`), verificando a lógica no código.

## 3. API JSON (`system_api_json.c`), OpenAPI (`openapi.yaml`), Mocks (`system.service.ts`) e Documentação

- [x] 3.1 Atualizar `main/http_server/system_api_json.c` para expor `throttleActive` (`0` ou `1` via `cJSON_AddNumberToObject`), `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount` e `watchdogResets`, enquanto `frequency` reporta `frequency_value` efetiva em RAM e `coreVoltage` reporta `actual_voltage_mv` comandada em RAM, verificando os campos gerados em `system_api_add_telemetry()` e `system_api_add_config()`.
- [x] 3.2 Atualizar `main/http_server/openapi.yaml` adicionando `throttleActive`, `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount` e `watchdogResets` em `SystemInfo` (e `dvfsProfile` em `Settings`), executar `npm run generate:api` em `main/http_server/axe-os` e atualizar o objeto mock em `main/http_server/axe-os/src/app/services/system.service.ts`, verificando a geração sem erros.
- [x] 3.3 Marcar os itens da Fase 4 como concluídos `[x]` no checklist da seção 5 de `doc/FIRMWARE_IMPROVEMENTS.md`, verificando o diff do arquivo.

## 4. Verificação de Testes Unitários e Build

- [x] 4.1 Executar `npm run test:ci` (ou `npm run build`) em `main/http_server/axe-os` e validar a compilação do firmware (`idf.py build` quando o ambiente ESP-IDF estiver ativo), garantindo zero erros de compilação e aprovação nos testes do frontend.

## Workflow follow-up

- Validar a implementação com `/opsx-explore` após a conclusão das tarefas.
- Arquivar a change `phase-4-vdroop-dvfs-and-axeos-telemetry` concluindo todas as fases do plano `doc/FIRMWARE_IMPROVEMENTS.md`.
