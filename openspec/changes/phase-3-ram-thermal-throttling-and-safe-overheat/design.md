# Design

## Context

Atualmente, `main/tasks/power_management_task.c` (linhas 241–363) possui apenas um gatilho abrupto de emergência (`THROTTLE_TEMP = 75.0f`, `TPS546_THROTTLE_TEMP = 105.0f`) que:
1. Sobrescreve configurações do usuário na flash NVS (`NVS_CONFIG_AUTO_FAN_SPEED = false`, `NVS_CONFIG_MANUAL_FAN_SPEED = 100`).
2. Aguarda um loop bloqueante até que `chip_temp_avg` caia para `65 °C` (`THROTTLE_TEMP - 10`) e então **reduz permanentemente** `NVS_CONFIG_ASIC_VOLTAGE` em `-100 mV` e `NVS_CONFIG_ASIC_FREQUENCY` em `-100 MHz` na NVS.
3. Não oferece uma zona intermediária de *Soft-Throttling* em RAM para conter subidas de temperatura (`68–74 °C` no ASIC ou `90–104 °C` no VR) ou excesso de potência de entrada (`> 32 W`) sem interromper a mineração.

Na Fase 2, já adicionamos `watchdog_freq_offset_mhz` e `last_freq_change_us` em `PowerManagementModule` (`main/tasks/power_management_task.h`). Agora na Fase 3, consolidaremos a separação entre **Target (NVS)** e **Efetivo (RAM)** tanto para frequência quanto para tensão VCore, integrando o Soft-Throttling térmico em RAM, a recuperação automática por histerese e o desligamento crítico seguro (`overheat_mode`) sem corromper as configurações do usuário na NVS.

## Goals / Non-Goals

**Goals:**
- Manter `NVS_CONFIG_ASIC_FREQUENCY` e `NVS_CONFIG_ASIC_VOLTAGE` exclusivamente como *Target* do usuário (`target_frequency`, `target_voltage`), aplicando offsets temporários apenas em RAM (`thermal_offset_mhz`, `thermal_offset_mv`, além do `watchdog_freq_offset_mhz` existente).
- Impor *Hard Caps* em RAM para proteção do hardware `601a-4` (`BM1370` + `TPS546D24A`): VCore efetivo limitado a `[1000 mV, 1250 mV]`, frequência efetiva em mineração limitada ao piso de `400.0 MHz` e teto de potência de entrada de `32.0 W`.
- Implementar a máquina de estados térmica em `power_management_task.c` com 3 zonas:
  1. **Emergência Crítica (`ASIC >= 75.0 °C` ou `VR >= 105.0 °C`)**: `mining_stop()`, cooler `100%` em RAM, `overheat_mode = 1`, zero escrita destrutiva nas chaves de tensão/frequência/fan da NVS.
  2. **Soft-Throttling em RAM (`ASIC >= 68.0 °C`, `VR >= 90.0 °C` ou `power > 32.0 W`)**: `throttle_active = true`, cooler `100%` em RAM, step-down a cada `10 s` (`-12.5 MHz` / `-10 mV`, ou `-25.0 MHz` / `-10 mV` se `ASIC >= 72.0 °C` ou `VR >= 98.0 °C`) até o piso de `400.0 MHz` / `1000 mV`.
  3. **Recuperação com Histerese (`ASIC < 65.0 °C`, `VR < 85.0 °C` e `power <= 32.0 W` contínuos por `30 s`)**: step-up a cada `15 s` (`+12.5 MHz` / `+10 mV`) até retornar ao `target_frequency` e `target_voltage`, limpando `throttle_active = false`.
- Atualizar `main/tasks/fan_controller_task.c` para forçar `100%` de PWM em RAM sempre que `power_management->throttle_active` ou `sys_module->overheat_mode` estiver ativo, sem alterar `NVS_CONFIG_AUTO_FAN_SPEED` nem `NVS_CONFIG_MANUAL_FAN_SPEED`.

**Non-Goals:**
- Compensação dinâmica de Vdroop sob carga e curva DVFS automática (escopo da Fase 4).
- Alteração do contrato JSON de `/api/system/info` e `openapi.yaml` para expor novos campos no frontend AxeOS (escopo da Fase 4 — item 7 de `doc/FIRMWARE_IMPROVEMENTS.md`).

## Decisions

### 1. Modelo de Estado Efetivo vs Target em `PowerManagementModule`
Adicionaremos ao `PowerManagementModule` em `main/tasks/power_management_task.h`:
- `float target_frequency;` — valor lido de `NVS_CONFIG_ASIC_FREQUENCY`.
- `uint16_t target_voltage;` — valor lido de `NVS_CONFIG_ASIC_VOLTAGE` (em mV).
- `uint16_t actual_voltage_mv;` — última tensão VCore efetiva enviada a `VCORE_set_voltage()` (em mV, `0` quando `mining_stop()` está ativo).
- `bool throttle_active;` — indica se o Soft-Throttling térmico/potência está ativo em RAM.
- `float thermal_offset_mhz;` — redução acumulada de frequência por Soft-Throttling em RAM (`>= 0.0f`).
- `uint16_t thermal_offset_mv;` — redução acumulada de tensão VCore por Soft-Throttling em RAM (`>= 0`).
- `int64_t last_throttle_step_us;` — timestamp (`esp_timer_get_time()`) do último step de redução ou recuperação térmica.
- `int64_t safe_zone_since_us;` — timestamp (`esp_timer_get_time()`) de quando o sistema entrou continuamente na zona segura de histerese (`0` se fora da zona segura).

**Cálculo dos valores efetivos em RAM a cada ciclo (quando `!sys_module->overheat_mode`):**
```text
target_voltage_clamped = clamp(target_voltage, 1000, 1250)
effective_voltage_mv   = max(1000, target_voltage_clamped - thermal_offset_mv)

effective_freq_mhz     = max(400.0f, target_frequency - watchdog_freq_offset_mhz - thermal_offset_mhz)
```
Se o usuário alterar `NVS_CONFIG_ASIC_FREQUENCY` ou `NVS_CONFIG_ASIC_VOLTAGE` via interface web durante a operação, `target_frequency` e `target_voltage` refletirão imediatamente o novo alvo na próxima iteração do loop, preservando eventuais offsets térmicos ativos até que a histerese os reduza.

### 2. Máquina de Estados Térmica em `power_management_task.c`

```text
+-----------------------------------------------------------------------+
| Leitura de Telemetria (ASIC Temp, VR Temp, Power)                     |
+-----------------------------------------------------------------------+
                                   |
                                   v
         [ ASIC >= 75 C  OU  VR >= 105 C (leituras validas) ]
                |                                      |
               SIM                                    NAO
                v                                      v
+-------------------------------+    [ ASIC >= 68 C OU VR >= 90 C OU Power > 32 W ]
| EMERGENCIA CRITICA            |           |                             |
| - mining_stop(GLOBAL_STATE)   |          SIM                           NAO
| - actual_voltage_mv = 0       |           v                             v
| - NVS OVERHEAT_MODE = true    |  +--------------------+   [ throttle_active == true? ]
| - sys->overheat_mode = true   |  | SOFT-THROTTLING    |         |              |
| - Fan 100% em RAM             |  | - throttle_active  |        SIM            NAO
| - NVS VCore/Freq INTACTOS     |  | - safe_zone = 0    |         v              v
+-------------------------------+  | - A cada 10s:      |  +--------------+  +--------+
                                   |   -12.5 ou -25 MHz |  | HISTERESE    |  | NORMAL |
                                   |   -10 mV VCore     |  | ASIC < 65 C  |  +--------+
                                   |   (piso 400/1000)  |  | VR < 85 C    |
                                   +--------------------+  | Power <= 32W |
                                                           | por >= 30s:  |
                                                           | +12.5 MHz e  |
                                                           | +10 mV / 15s |
                                                           +--------------+
```

- **Validação de leitura antes de acionar throttling/overheat**:
  - Assim como no código atual (`(vr_temp > TPS546_THROTTLE_TEMP || chip_temp_avg > THROTTLE_TEMP) && (vr_temp > 0.0f || chip_temp_avg > 0.0f)`), ignoramos leituras inválidas (`isnan()` ou `<= 0.0f`).
- **Transição para Emergência Crítica (`chip_temp_avg >= 75.0f || vr_temp >= 105.0f`)**:
  - Chama `mining_stop(GLOBAL_STATE)`, zera `actual_voltage_mv = 0`, zera `thermal_offset_mhz = 0.0f`, `thermal_offset_mv = 0`, `throttle_active = false`, `safe_zone_since_us = 0`, e grava apenas `nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, true)` e `sys_module->overheat_mode = true`.
  - Não executa nenhum loop bloqueante `while (chip_temp_avg > ...)` dentro do bloco de emergência: o próprio loop principal de `power_management_task` continua rodando a cada `POLL_RATE` (`1800 ms`), atualizando telemetria de temperatura/potência enquanto `sys_module->overheat_mode == true` mantém a mineração parada até que o usuário limpe `overheat_mode` no AxeOS (e as temperaturas estejam seguras).
- **Retomada após limpeza de `overheat_mode`**:
  - Quando `nvs_overheat_mode == false` e `sys_module->overheat_mode == true`, só permite `mining_start(GLOBAL_STATE)` se `chip_temp_avg < 65.0f` e `vr_temp < 85.0f` (caso contrário, se ainda estiver acima do limiar crítico, reativa `overheat_mode`). Ao chamar `mining_start(GLOBAL_STATE)`, reaplica `effective_voltage_mv` e `effective_freq_mhz` a partir dos targets intactos da NVS.

### 3. Controle de Fan em RAM (`fan_controller_task.c`)
Em `main/tasks/fan_controller_task.c`:
- Atualmente (linha 71): `if (nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE))` força `fan_perc = 100.0f`.
- Modificaremos a condição de prioridade máxima para verificar:
  `if (GLOBAL_STATE->SYSTEM_MODULE.overheat_mode || GLOBAL_STATE->POWER_MANAGEMENT_MODULE.throttle_active || nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE))`
  forçando `power_management->fan_perc = 100.0f` em RAM (status `"Overheat"` quando em `overheat_mode`, ou `"Throttle"` / `"Overheat"` para log) sem tocar em `NVS_CONFIG_AUTO_FAN_SPEED` ou `NVS_CONFIG_MANUAL_FAN_SPEED`.
- Assim que `throttle_active` voltar a `false` (e `overheat_mode` for `false`), o `else if (nvs_config_get_bool(NVS_CONFIG_AUTO_FAN_SPEED))` retoma automaticamente o controle PID (ou manual, conforme preferência original do usuário na NVS).

## Risks / Trade-offs

- **[Risk] Oscilação (ping-pong) entre Soft-Throttling e Recuperação** -> **Mitigação**: Banda morta (deadband) de `3 °C` no ASIC (`68 °C` para entrar em throttling vs `< 65 °C` para recuperar) e `5 °C` no VR (`90 °C` vs `< 85 °C`), combinada com janela obrigatória de `30 s` contínuos na zona segura antes do primeiro passo de subida e passos lentos a cada `15 s`.
- **[Risk] Mudança manual de tensão/frequência pelo usuário durante Soft-Throttling** -> **Mitigação**: Ao separar `target_voltage` / `target_frequency` de `thermal_offset_mv` / `thermal_offset_mhz`, qualquer alteração feita pelo usuário na UI atualiza o target imediatamente, enquanto a redução térmica (`thermal_offset_*`) continua protegendo o chip até a temperatura baixar. Se a redução acumulada exceder `target - floor`, ela é limitada a `target - floor` para não atrasar a recuperação.
- **[Risk] Chamadas repetidas a `VCORE_set_voltage()` sem mudança real de tensão** -> **Mitigação**: Rastrear `actual_voltage_mv` em `PowerManagementModule` e chamar `VCORE_set_voltage()` apenas quando `effective_voltage_mv != power_management->actual_voltage_mv`.
