# Proposal

## Why

O loop de gerenciamento de energia atual em `main/tasks/power_management_task.c` possui apenas um limiar rígido de emergência (`75 °C` no ASIC / `105 °C` no VR) que corrompe permanentemente as configurações do usuário em NVS (forçando `NVS_CONFIG_AUTO_FAN_SPEED = false`, `NVS_CONFIG_MANUAL_FAN_SPEED = 100` e reduzindo `NVS_CONFIG_ASIC_VOLTAGE` / `NVS_CONFIG_ASIC_FREQUENCY` em `-100 mV` / `-100 MHz`), além de não possuir regulação térmica progressiva (Soft-Throttling) antes do desligamento crítico nem retorno automático por histerese em RAM.

## What Changes

- **Separação entre Target NVS e Efetivo em RAM + Hard Caps**:
  - Introduzir variáveis de estado efetivo em RAM no `PowerManagementModule` (`target_frequency`, `target_voltage`, `actual_voltage_mv`, `throttle_active`, `thermal_offset_mhz`, `thermal_offset_mv`) mantendo os valores salvos na NVS estritamente como *Target* do usuário.
  - Garantir tetos e pisos de segurança (*Hard Caps*) na aplicação efetiva de VCore (`1000 mV` a `1250 mV`), piso de frequência em throttling (`400 MHz`) e teto de potência de entrada (`32.0 W`).
- **Soft-Throttling Progressivo em RAM com Histerese**:
  - Na Zona de Soft-Throttling (`chip_temp_avg >= 68.0 °C`, `vr_temp >= 90.0 °C` ou `power > 32.0 W`), ativar `throttle_active = true`, forçar o cooler a `100%` apenas em RAM (sem alterar `NVS_CONFIG_AUTO_FAN_SPEED` ou `NVS_CONFIG_MANUAL_FAN_SPEED`) e aplicar redução gradual a cada `10 s` de `-12.5 MHz` na frequência em RAM (`-25.0 MHz` se `chip_temp_avg >= 72.0 °C` ou `vr_temp >= 98.0 °C`) e `-10 mV` no VCore, respeitando o piso de `400.0 MHz` e `1000 mV`.
  - Na Zona de Recuperação com Histerese (`chip_temp_avg < 65.0 °C`, `vr_temp < 85.0 °C` e `power <= 32.0 W` de forma contínua por `30 s`), incrementar gradualmente a frequência em RAM em `+12.5 MHz` e o VCore em `+10 mV` a cada `15 s` até retornar ao *Target* configurado na NVS, desativando `throttle_active = false` e devolvendo o controle do cooler ao modo configurado pelo usuário (PID automático ou manual).
- **Emergência Crítica (Safe Overheat Shutdown) sem Corrupção de NVS**:
  - Caso `chip_temp_avg >= 75.0 °C` ou `vr_temp >= 105.0 °C`, acionar `mining_stop(GLOBAL_STATE)` (`VCore = 0 V`, `RST LOW`, parada de envio de jobs), manter o cooler a `100%` em RAM e ativar `overheat_mode = 1` até intervenção do usuário.
  - **Remover completamente** qualquer escrita em `NVS_CONFIG_ASIC_VOLTAGE`, `NVS_CONFIG_ASIC_FREQUENCY`, `NVS_CONFIG_AUTO_FAN_SPEED` e `NVS_CONFIG_MANUAL_FAN_SPEED` de dentro do loop térmico de `power_management_task.c`, preservando intactas as configurações originais do usuário na flash NVS.

## Capabilities

### New Capabilities
- `ram-thermal-throttling-and-safe-overheat`: Separação entre Target NVS e estado efetivo em RAM, Hard Caps de segurança (`1000–1250 mV`, `400 MHz`, `32 W`), Soft-Throttling progressivo em RAM com recuperação por histerese e desligamento crítico de emergência (`overheat_mode`) sem sobrescrever configurações de tensão, frequência ou fan na NVS.

### Modified Capabilities
<!-- Nenhuma capability existente em openspec/specs/ tem seus requisitos alterados por esta fase. -->

## Impact

- **Código Afetado**:
  - `main/tasks/power_management_task.h` (novos campos de estado efetivo/target e controle de throttling em `PowerManagementModule`).
  - `main/tasks/power_management_task.c` (máquina de estados de Soft-Throttling em RAM, histerese de recuperação, Hard Caps e remoção das escritas destrutivas em NVS no bloco de overheat).
  - `main/tasks/fan_controller_task.c` (forçar `100%` de PWM em RAM quando `throttle_active` ou `overheat_mode` estiver ativo, sem modificar chaves de fan na NVS).
  - `doc/FIRMWARE_IMPROVEMENTS.md` (atualização do checklist da Fase 3 após implementação).
- **APIs / Persistência**:
  - As chaves NVS (`asicvoltage`, `asicfrequency`, `autofanspeed`, `manualfanspeed`) deixam de ser sobrescritas por eventos térmicos. `overheat_mode` continua sendo sinalizado para bloqueio de segurança até limpeza explícita pelo usuário via AxeOS (`PATCH /api/system`).
