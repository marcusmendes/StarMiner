# Tasks

## 1. Estrutura de Estado Efetivo vs Target e Hard Caps (`PowerManagementModule`)

- [x] 1.1 Adicionar em `main/tasks/power_management_task.h` (struct `PowerManagementModule`) os campos de separação entre Target (NVS) e Efetivo (RAM) e controle de Soft-Throttling (`float target_frequency`, `uint16_t target_voltage`, `uint16_t actual_voltage_mv`, `bool throttle_active`, `float thermal_offset_mhz`, `uint16_t thermal_offset_mv`, `int64_t last_throttle_step_us`, `int64_t safe_zone_since_us`), verificando a presença de todos os campos no header.
- [x] 1.2 Definir constantes de Hard Caps e limiares térmicos em `main/tasks/power_management_task.c` (`VCORE_MIN_MV = 1000`, `VCORE_MAX_MV = 1250`, `FREQ_MIN_MHZ = 400.0f`, `MAX_POWER_W = 32.0f`, `ASIC_SOFT_THROTTLE_TEMP = 68.0f`, `ASIC_SEVERE_THROTTLE_TEMP = 72.0f`, `ASIC_EMERGENCY_TEMP = 75.0f`, `ASIC_SAFE_RECOVERY_TEMP = 65.0f`, `VR_SOFT_THROTTLE_TEMP = 90.0f`, `VR_SEVERE_THROTTLE_TEMP = 98.0f`, `VR_EMERGENCY_TEMP = 105.0f`, `VR_SAFE_RECOVERY_TEMP = 85.0f`) e inicializar os novos campos de `power_management` antes do loop principal, verificando por inspeção em `main/tasks/power_management_task.c`.

## 2. Máquina de Estados de Soft-Throttling em RAM, Histerese e Emergência Sem Corrupção de NVS

- [x] 2.1 Substituir o bloco legado de overheat em `main/tasks/power_management_task.c` pela zona de Emergência Crítica (`chip_temp_avg >= 75.0f || vr_temp >= 105.0f` com leitura válida): chamar `mining_stop(GLOBAL_STATE)`, zerar `actual_voltage_mv = 0` e offsets térmicos, gravar `nvs_config_set_bool(NVS_CONFIG_OVERHEAT_MODE, true)` e `sys_module->overheat_mode = true`, e remover todas as chamadas `nvs_config_set_bool(NVS_CONFIG_AUTO_FAN_SPEED, ...)`, `nvs_config_set_u16(NVS_CONFIG_MANUAL_FAN_SPEED, ...)`, `nvs_config_set_u16(NVS_CONFIG_ASIC_VOLTAGE, ...)` e `nvs_config_set_float(NVS_CONFIG_ASIC_FREQUENCY, ...)`, verificando com `grep` que nenhuma escrita nessas 4 chaves NVS permanece em `main/tasks/power_management_task.c`.
- [x] 2.2 Implementar em `main/tasks/power_management_task.c` a zona de Soft-Throttling em RAM (`chip_temp_avg >= 68.0f || vr_temp >= 90.0f || power > 32.0f` quando não em `overheat_mode`): ativar `power_management->throttle_active = true`, zerar `safe_zone_since_us = 0`, e a cada `10 s` (`10000000LL` us) incrementar `thermal_offset_mhz` em `+12.5f` (`+25.0f` se `chip_temp_avg >= 72.0f || vr_temp >= 98.0f`) e `thermal_offset_mv` em `+10`, limitando a redução ao piso efetivo de `400.0 MHz` e `1000 mV`, verificando a lógica e os logs `ESP_LOGW` no código.
- [x] 2.3 Implementar em `main/tasks/power_management_task.c` a zona de Recuperação com Histerese (`chip_temp_avg < 65.0f && vr_temp < 85.0f && power <= 32.0f` quando `throttle_active == true`): contabilizar `safe_zone_since_us` e, após `30 s` (`30000000LL` us) contínuos na zona segura, reduzir `thermal_offset_mhz` em `12.5f` e `thermal_offset_mv` em `10` a cada `15 s` (`15000000LL` us) até `0`, limpando `throttle_active = false` quando ambos os offsets chegarem a zero, verificando a lógica no código.
- [x] 2.4 Atualizar a aplicação de VCore e frequência em `main/tasks/power_management_task.c` para ler `target_voltage` e `target_frequency` da NVS, aplicar Hard Caps (`1000–1250 mV` e piso `400.0 MHz`), calcular os valores efetivos subtraindo `thermal_offset_mv` e `thermal_offset_mhz + watchdog_freq_offset_mhz`, aplicar mudanças apenas quando divergirem de `actual_voltage_mv` / `frequency_value`, e garantir que a saída de `overheat_mode` só chame `mining_start(GLOBAL_STATE)` se as temperaturas estiverem na zona segura (`< 65.0 °C` ASIC e `< 85.0 °C` VR), verificando a transição completa no código.

## 3. Controle do Cooler em RAM (`fan_controller_task.c`) e Documentação

- [x] 3.1 Atualizar `main/tasks/fan_controller_task.c` para forçar `power_management->fan_perc = 100.0f` em RAM quando `GLOBAL_STATE->SYSTEM_MODULE.overheat_mode || GLOBAL_STATE->POWER_MANAGEMENT_MODULE.throttle_active || nvs_config_get_bool(NVS_CONFIG_OVERHEAT_MODE)` estiver ativo, sem alterar `NVS_CONFIG_AUTO_FAN_SPEED` nem `NVS_CONFIG_MANUAL_FAN_SPEED`, retomando automaticamente o modo PID ou manual quando `throttle_active` e `overheat_mode` forem `false`, verificando a prioridade de ramos em `main/tasks/fan_controller_task.c`.
- [x] 3.2 Marcar os itens concluídos da Fase 3 no checklist da seção 10 de `doc/FIRMWARE_IMPROVEMENTS.md`, verificando o diff do arquivo.

## 4. Verificação de Compilação e Integração

- [x] 4.1 Compilar o projeto (ou validar a compilação dos módulos modificados `power_management_task.c` e `fan_controller_task.c`) garantindo zero erros ou warnings de compilação.

## Workflow follow-up

- Validar a implementação com `/opsx-explore` ou `/opsx-archive` após a conclusão das tarefas.
- Arquivar a change `phase-3-ram-thermal-throttling-and-safe-overheat` antes de iniciar a Fase 4 (`phase-4-vdroop-dvfs-and-axeos-telemetry`).
