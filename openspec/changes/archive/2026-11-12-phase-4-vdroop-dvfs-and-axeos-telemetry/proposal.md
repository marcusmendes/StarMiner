# Proposal

## Why

Sob carga elevada no `BM1370`, a resistência das trilhas e indutores entre o `TPS546D24A` e o ASIC causa queda resistiva (*Vdroop*), reduzindo a tensão real no die abaixo da tensão alvo e aumentando erros de hash ou travamentos de domínios; além disso, o firmware ainda não oferece perfis de auto-ajuste DVFS (`Manual`, `Eficiência`, `Equilibrado`, `Performance`) nem expõe os novos indicadores de proteção térmica, *Vdroop*, erros `STATUS_CML` e resets de watchdog na API HTTP/WebSocket e no frontend Angular (`AxeOS`).

## What Changes

- **Compensação de *Vdroop* em Malha Lenta (`10 s`)**:
  - A cada `10 s` em operação estável (ASIC inicializado, fora de `throttle_active` e `overheat_mode`, e após estabilização de frequência), comparar `core_voltage` medido (`VCORE_get_voltage_mv()`) com a tensão base pretendida (`base_voltage_mv`, antes da compensação de *Vdroop*).
  - Se houver queda resistiva (`core_voltage < base_voltage_mv - 5 mV`), incrementar `vdroop_offset_mv` em passos de `+5 mV` até o teto máximo de `+20 mV` (ou reduzir em `-5 mV` até `0 mV` se `core_voltage > base_voltage_mv + 5 mV`), respeitando sempre os *Hard Caps* (`1250 mV` e `32.0 W`).
- **Perfis de Operação DVFS / Auto-Tuner (`NVS_CONFIG_DVFS_PROFILE`)**:
  - Adicionar a chave `NVS_CONFIG_DVFS_PROFILE` (`"dvfsprofile"`, `TYPE_U16`, padrão `0`, `rest_name = "dvfsProfile"`, `min = 0`, `max = 3`) em `main/nvs_config.h` e `main/nvs_config.c`.
  - Implementar os 4 perfis de operação em `main/tasks/power_management_task.c` (avaliados a cada `60 s` quando fora de `throttle_active` e `overheat_mode`, respeitando os *Hard Caps* `1000–1250 mV`, `400–675 MHz` e `32.0 W`):
    - **`0` — `Manual`**: segue `target_frequency` e `target_voltage` da NVS com *Soft-Throttling*, *Vdroop* e Watchdog ativos.
    - **`1` — `Eficiência` (`500–600 MHz`, erro `<= 0,5%`)**: ajusta `VCore` em passos de `-5 mV` a cada `60 s` enquanto `error_percentage <= 0.5%` até encontrar o menor ponto estável (recuperando `+10 mV` se `error_percentage > 0.5%`).
    - **`2` — `Equilibrado` (`525–625 MHz`, erro `<= 0,8%`)**: escala o clock em passos de `+12.5 MHz` a cada `60 s` enquanto `chip_temp_avg < 64.0 °C`, `vr_temp < 80.0 °C` e `error_percentage <= 0.8%` (recuando `-12.5 MHz` se `error_percentage > 0.8%`).
    - **`3` — `Performance` (`550–675 MHz`, erro `<= 1,0%`)**: escala clock (`+12.5 MHz`) e `VCore` (`+10 mV` quando `error_percentage > 1.0%` e `VCore < 1250 mV`) até encontrar o primeiro limite (`68.0 °C` ASIC, `90.0 °C` VR, `1250 mV`, `32.0 W` ou `1.0%` de erro).
- **Exposição na API HTTP (`GET /api/system/info` e `PATCH /api/system`), OpenAPI e Frontend (`AxeOS`)**:
  - Atualizar `main/http_server/system_api_json.c` para expor:
    - `throttleActive` (`integer`: `0` ou `1` via `cJSON_AddNumberToObject`)
    - `targetFrequency` (`number`: frequência alvo da NVS em MHz, enquanto `frequency` reflete `frequency_value` efetiva atual em RAM)
    - `targetVoltage` (`integer`: tensão alvo da NVS em mV, enquanto `coreVoltage` reflete a tensão comandada atual em RAM)
    - `vdroopOffsetMv` (`integer`: compensação de *Vdroop* aplicada em mV)
    - `dvfsProfile` (`integer`: `0..3`)
    - `cmlErrorCount` (`integer`: contador `cml_error_count` de eventos `TPS546_STATUS_CML`)
    - `watchdogResets` (`integer`: contador `watchdog_resets` da sessão)
  - Atualizar `main/http_server/openapi.yaml` (schemas `SystemInfo` e `Settings`), regenerar o cliente TypeScript (`npm run generate:api` em `main/http_server/axe-os`), atualizar o objeto mock em `main/http_server/axe-os/src/app/services/system.service.ts` e validar com `npm run test:ci`.

## Capabilities

### New Capabilities
- `vdroop-and-dvfs-autotuner`: Compensação automática de queda resistiva (*Vdroop*) em malha lenta (`10 s`, teto `+20 mV`) e máquina de estados dos 4 perfis DVFS (`Manual`, `Eficiência`, `Equilibrado`, `Performance`) em RAM dentro dos *Hard Caps*.
- `axeos-telemetry-and-dvfs-api`: Persistência NVS e configuração via `PATCH /api/system` para `dvfsProfile`, exposição de telemetria estendida (`throttleActive`, `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount`, `watchdogResets`) em `/api/system/info`, contrato `openapi.yaml` e paridade de mocks no frontend Angular `AxeOS`.

### Modified Capabilities
<!-- Nenhuma capability existente em openspec/specs/ tem seus requisitos alterados por esta fase. -->

## Impact

- **Código Afetado**:
  - `main/nvs_config.h` e `main/nvs_config.c` (`NVS_CONFIG_DVFS_PROFILE`).
  - `main/tasks/power_management_task.h` e `main/tasks/power_management_task.c` (compensação de *Vdroop* e perfis DVFS).
  - `main/http_server/system_api_json.c` (novos campos em `system_api_add_telemetry` e `system_api_add_config`).
  - `main/http_server/openapi.yaml`, `main/http_server/axe-os/src/app/generated/**` e `main/http_server/axe-os/src/app/services/system.service.ts`.
  - `doc/FIRMWARE_IMPROVEMENTS.md` (conclusão do checklist da Fase 4).
