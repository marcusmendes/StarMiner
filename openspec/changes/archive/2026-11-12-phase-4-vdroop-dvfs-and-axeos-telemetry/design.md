# Design

## Context

Com a conclusão da Fase 3, `PowerManagementModule` (`main/tasks/power_management_task.h`) separa o estado alvo da NVS (`target_frequency`, `target_voltage`) do estado efetivo em RAM (`frequency_value`, `actual_voltage_mv`, `thermal_offset_mhz`, `thermal_offset_mv`, `watchdog_freq_offset_mhz`, `throttle_active`) dentro dos *Hard Caps* (`1000–1250 mV`, `400.0 MHz`, `32.0 W`).

Para concluir o plano de melhorias do firmware (`doc/FIRMWARE_IMPROVEMENTS.md`, seções 4.7, 4.8 e 4.9), precisamos adicionar:
1. Compensação de *Vdroop* em malha lenta (`10 s`, teto `+20 mV`).
2. Perfis de auto-ajuste DVFS (`0 = Manual`, `1 = Eficiência`, `2 = Equilibrado`, `3 = Performance`) persistidos via `NVS_CONFIG_DVFS_PROFILE` (`dvfsProfile`).
3. Exposição de toda a telemetria avançada no endpoint `GET /api/system/info` (`main/http_server/system_api_json.c`), suporte a `dvfsProfile` em `PATCH /api/system`, atualização de `main/http_server/openapi.yaml`, regeneração da API TypeScript e atualização dos mocks em `main/http_server/axe-os/src/app/services/system.service.ts`.

## Goals / Non-Goals

**Goals:**
- Compensar quedas resistivas (*Vdroop*) entre o `TPS546D24A` e o `BM1370` com passos de `+5 mV` a cada `10 s` até o limite de `+20 mV`, nunca ultrapassando `1250 mV` ou `32.0 W`.
- Implementar os 4 perfis DVFS em RAM sem sobrescrever `NVS_CONFIG_ASIC_FREQUENCY` ou `NVS_CONFIG_ASIC_VOLTAGE` na flash NVS.
- Expor os 7 novos campos (`throttleActive`, `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount`, `watchdogResets`) em `GET /api/system/info` mantendo estrita compatibilidade com as regras do `AGENTS.md` (inteiro `0/1` para flags como `throttleActive`, geração via `npm run generate:api` e paridade de mocks em `system.service.ts`).

**Non-Goals:**
- Criar novas telas ou alterar componentes visuais HTML/SCSS do Angular além da atualização do schema OpenAPI, cliente gerado e serviço de mocks (`system.service.ts`), conforme escopo definido na seção 4.9 e no checklist da Fase 4 em `doc/FIRMWARE_IMPROVEMENTS.md`.

## Decisions

### 1. Composição da Tensão Base, *Vdroop* e Perfis DVFS em `PowerManagementModule`

Adicionaremos em `PowerManagementModule` (`main/tasks/power_management_task.h`):
- `uint16_t vdroop_offset_mv;` — compensação atual de *Vdroop* em mV (`0..20`).
- `int64_t last_vdroop_check_us;` — timestamp (`esp_timer_get_time()`) da última avaliação de *Vdroop*.
- `uint16_t dvfs_profile;` — perfil DVFS ativo (`0 = Manual`, `1 = Eficiência`, `2 = Equilibrado`, `3 = Performance`).
- `float dvfs_freq_offset_mhz;` — ajuste de frequência em MHz calculado pelo Auto-Tuner DVFS em relação ao target (`0.0f` no modo `Manual`).
- `int16_t dvfs_voltage_offset_mv;` — ajuste de tensão em mV calculado pelo Auto-Tuner DVFS em relação ao target (`0` no modo `Manual`).
- `int64_t last_dvfs_step_us;` — timestamp (`esp_timer_get_time()`) do último passo de auto-ajuste DVFS (intervalo de `60 s`).

```text
+-------------------------------------------------------------------------+
| Target NVS: target_frequency (MHz), target_voltage (mV)                 |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| Perfil DVFS (0=Manual, 1=Eficiencia, 2=Equilibrado, 3=Performance)      |
| -> base_target_freq = clamp_profile(target_frequency + dvfs_freq_ofs)   |
| -> base_target_mv   = clamp(target_voltage + dvfs_voltage_ofs,1000,1250)|
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| Reducoes de Protecao (Thermal Soft-Throttling + Watchdog)               |
| -> effective_freq_mhz = max(400.0, base_target_freq - thermal - wdog)   |
| -> base_voltage_mv    = clamp(base_target_mv - thermal_offset_mv,       |
|                               1000, 1250)                               |
+-------------------------------------------------------------------------+
                                    |
                                    v
+-------------------------------------------------------------------------+
| Compensacao de Vdroop em Malha Lenta (a cada 10s, teto +20 mV)          |
| -> effective_voltage_mv = clamp(base_voltage_mv + vdroop_offset_mv,     |
|                                 1000, 1250)                             |
+-------------------------------------------------------------------------+
```

### 2. Algoritmo de Compensação de *Vdroop* (`10 s`)
- Condições para avaliar *Vdroop*:
  - `GLOBAL_STATE->ASIC_initalized == true`
  - `!sys_module->overheat_mode && !power_management->throttle_active`
  - `power_management->core_voltage > 500.0f` (leitura válida em mV)
  - `(now_us - power_management->last_freq_change_us) >= 10000000LL` (`10 s` após transição de clock)
  - `(now_us - power_management->last_vdroop_check_us) >= 10000000LL` (a cada `10 s`)
- Seja `base_voltage_mv` a tensão pretendida no die (após perfil DVFS e offset térmico, antes do `vdroop_offset_mv`):
  - Se `power_management->core_voltage < ((float)base_voltage_mv - 5.0f)` e `power_management->power <= 32.0f` e `base_voltage_mv + power_management->vdroop_offset_mv < 1250`:
    - `vdroop_offset_mv = min(20, vdroop_offset_mv + 5)`.
  - Caso contrário, se `power_management->core_voltage > ((float)base_voltage_mv + 5.0f)` e `vdroop_offset_mv >= 5`:
    - `vdroop_offset_mv -= 5`.
- Sempre que `throttle_active` ativar, `overheat_mode` ativar ou `mining_stop()` for chamado, `vdroop_offset_mv` é limitado/zerado conforme necessário para garantir que a proteção térmica tenha prioridade absoluta.

### 3. Máquina de Estados dos Perfis DVFS (`60 s`)
- Persistência em NVS:
  - Em `main/nvs_config.h`: adicionar `NVS_CONFIG_DVFS_PROFILE` antes de `NVS_CONFIG_COUNT`.
  - Em `main/nvs_config.c`:
    `[NVS_CONFIG_DVFS_PROFILE] = {.nvs_key_name = "dvfsprofile", .type = TYPE_U16, .default_value = {.u16 = 0}, .rest_name = "dvfsProfile", .min = 0, .max = 3}`
  - Como `check_settings_and_update()` em `main/http_server/http_server.c` itera sobre todos os `NvsConfigKey` que possuem `.rest_name`, o endpoint `PATCH /api/system` passa a validar (`0..3`) e salvar `dvfsProfile` automaticamente sem duplicação de código.
- Ao mudar `dvfs_profile` (ou alterar `target_frequency` / `target_voltage` na NVS), zeramos `dvfs_freq_offset_mhz = 0.0f`, `dvfs_voltage_offset_mv = 0` e reiniciamos `last_dvfs_step_us = now_us`.
- A cada `60 s` (`60000000LL` us) em operação normal (`ASIC_initalized`, `!throttle_active`, `!overheat_mode`):
  - **Perfil `0` (`Manual`)**: `dvfs_freq_offset_mhz = 0.0f`, `dvfs_voltage_offset_mv = 0`.
  - **Perfil `1` (`Eficiência`, `500–600 MHz`, erro `<= 0,5%`)**:
    - Garante frequência base em `[500.0, 600.0] MHz`.
    - Se `sys_module->error_percentage <= 0.5f`, reduz `dvfs_voltage_offset_mv` em `-5 mV` (respeitando `base_target_mv >= 1000 mV`).
    - Se `sys_module->error_percentage > 0.5f`, eleva `dvfs_voltage_offset_mv` em `+10 mV` (respeitando `base_target_mv <= 1250 mV`).
  - **Perfil `2` (`Equilibrado`, `525–625 MHz`, erro `<= 0,8%`)**:
    - Garante frequência base inicial em `[525.0, 625.0] MHz`.
    - Se `chip_temp_avg < 64.0f`, `vr_temp < 80.0f`, `power <= 32.0f` e `error_percentage <= 0.8f`, incrementa `dvfs_freq_offset_mhz` em `+12.5 MHz` até o teto de `625.0 MHz`.
    - Se `error_percentage > 0.8f`, reduz `dvfs_freq_offset_mhz` em `-12.5 MHz` até o piso de `525.0 MHz`.
  - **Perfil `3` (`Performance`, `550–675 MHz`, erro `<= 1,0%`)**:
    - Garante frequência base inicial em `[550.0, 675.0] MHz`.
    - Se `error_percentage > 1.0f`: se `base_target_mv < 1250 mV` e `power < 31.0f`, eleva `dvfs_voltage_offset_mv` em `+10 mV`; caso já esteja no limite de tensão/potência, reduz `dvfs_freq_offset_mhz` em `-12.5 MHz` (até `550.0 MHz`).
    - Se `error_percentage <= 1.0f`, `chip_temp_avg < 68.0f`, `vr_temp < 90.0f` e `power < 31.0f`: incrementa `dvfs_freq_offset_mhz` em `+12.5 MHz` até `675.0 MHz`.

### 4. Contrato JSON (`/api/system/info`), OpenAPI e Mocks do AxeOS
Em `main/http_server/system_api_json.c`:
- Em `system_api_add_telemetry()`:
  - `cJSON_AddNumberToObject(root, "throttleActive", g->POWER_MANAGEMENT_MODULE.throttle_active ? 1 : 0);`
  - `cJSON_AddNumberToObject(root, "vdroopOffsetMv", g->POWER_MANAGEMENT_MODULE.vdroop_offset_mv);`
  - `cJSON_AddNumberToObject(root, "cmlErrorCount", g->POWER_MANAGEMENT_MODULE.cml_error_count);`
  - `cJSON_AddNumberToObject(root, "watchdogResets", g->POWER_MANAGEMENT_MODULE.watchdog_resets);`
- Em `system_api_add_config()`:
  - `cJSON_AddFloatToObject(root, "targetFrequency", g->POWER_MANAGEMENT_MODULE.target_frequency > 0.0f ? g->POWER_MANAGEMENT_MODULE.target_frequency : nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY));`
  - `cJSON_AddNumberToObject(root, "targetVoltage", g->POWER_MANAGEMENT_MODULE.target_voltage > 0 ? g->POWER_MANAGEMENT_MODULE.target_voltage : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE));`
  - `cJSON_AddNumberToObject(root, "coreVoltage", g->POWER_MANAGEMENT_MODULE.actual_voltage_mv > 0 ? g->POWER_MANAGEMENT_MODULE.actual_voltage_mv : nvs_config_get_u16(NVS_CONFIG_ASIC_VOLTAGE));`
  - `cJSON_AddFloatToObject(root, "frequency", g->POWER_MANAGEMENT_MODULE.frequency_value > 0.0f ? g->POWER_MANAGEMENT_MODULE.frequency_value : nvs_config_get_float(NVS_CONFIG_ASIC_FREQUENCY));`
  - `cJSON_AddNumberToObject(root, "dvfsProfile", nvs_config_get_u16(NVS_CONFIG_DVFS_PROFILE));`

Em `main/http_server/openapi.yaml`:
- Adicionar `throttleActive`, `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount` e `watchdogResets` em `components.schemas.SystemInfo` (e na lista `required` de `SystemInfo`).
- Adicionar `dvfsProfile` (`type: integer`, `minimum: 0`, `maximum: 3`) em `components.schemas.Settings`.
- Executar `npm run generate:api` em `main/http_server/axe-os` e atualizar o objeto retornado por `getInfo()` em `main/http_server/axe-os/src/app/services/system.service.ts`.

## Risks / Trade-offs

- **[Risk] Conflito entre Auto-Tuner DVFS e Soft-Throttling térmico** -> **Mitigação**: O Auto-Tuner DVFS e a compensação de *Vdroop* ficam congelados sempre que `throttle_active` ou `overheat_mode` estiverem ativos, garantindo prioridade absoluta à malha de proteção térmica.
- **[Risk] `edit.component.ts` usa `info.coreVoltage` e `info.frequency` ao abrir a tela de Settings durante um evento de throttling** -> **Mitigação**: No perfil padrão `0` (`Manual`) fora de throttling/Vdroop, `coreVoltage` e `frequency` coincidem com `targetVoltage` e `targetFrequency`, enquanto `targetVoltage` e `targetFrequency` expõem explicitamente os alvos da NVS mesmo durante ajustes dinâmicos em RAM.
