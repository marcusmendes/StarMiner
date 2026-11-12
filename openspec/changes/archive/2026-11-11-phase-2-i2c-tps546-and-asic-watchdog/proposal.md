# Proposal

## Why

O diagnóstico em operação na placa StarMiner (`601a-4` / `BM1370`) registrou falhas `TPS546D24A STATUS_CML=0x02 COMM` causadas por 30 transações I2C/s redundantes no barramento `I2C0` (releitura constante de `PMBUS_VOUT_MODE` e dupla leitura de `PMBUS_READ_VOUT` a cada 100 ms), além da ausência de `i2c_master_bus_reset` em falhas transitórias e de recuperação automática caso o `BM1370` congele em `0 GH/s` ou tenha um de seus 4 domínios travado sob *undervolt/overclock*. Esta change implementa a **Fase 2** de `doc/FIRMWARE_IMPROVEMENTS.md`.

## What Changes

- **Otimização de Tráfego I2C e Recuperação de `STATUS_CML` no `TPS546D24A` (Seção 4.3):**
  - Cache estático do registrador `PMBUS_VOUT_MODE` (`0x20`) durante `TPS546_init()`, eliminando leituras I2C dentro de `ulinear16_2_float()` e `float_2_ulinear16()` (`main/power/TPS546.c`).
  - Eliminação da leitura dupla de `PMBUS_READ_VOUT` a cada 100 ms entre `Power_get_output()` e `VCORE_get_voltage_mv()`, reaproveitando a tensão de saída recém-lida no mesmo ciclo de telemetria (`main/power/TPS546.c`, `main/power/TPS546.h`, `main/power/vcore.c`).
  - Tratamento não-fatal de `TPS546_STATUS_CML` em `TPS546_check_status()`: leitura de `PMBUS_STATUS_CML` (`0x7E`), incremento de `POWER_MANAGEMENT_MODULE.cml_error_count`, emissão de `ESP_LOGW` e envio de `PMBUS_CLEAR_FAULTS` (`0x03`) sem acionar `power_fault` quando não houver falha elétrica/térmica crítica (`OFF | VOUT_OV | IOUT_OC | VIN_UV | TEMP`).
  - Invocação de `i2c_master_bus_reset(i2c_bus_handle)` em `i2c_transfer_with_retries()` (`main/i2c_bitaxe.c`) quando uma transferência falhar antes da última tentativa, destravando escravos I2C presos em `SDA LOW`.
- **Watchdog de `0 GH/s` e Domínios Congelados em 3 Níveis (Seção 4.4):**
  - Monitoramento contínuo em `hashrate_monitor_task` (`main/tasks/hashrate_monitor_task.c`) armado quando `ASIC_initalized == true`, Stratum ativo (`SYSTEM_MODULE.work_received > 0` e `!SYSTEM_MODULE.pools_unavailable`), sem falha térmica/elétrica (`overheat_mode == 0`, `power_fault == 0`, `!hardware_fault`), fora de self-test e após janela de carência de `30 s` pós-inicialização ou transição de frequência.
  - Disparo do gatilho `POWER_MANAGEMENT_MODULE.asic_watchdog_trigger = true` quando:
    1. **Hashrate Total Zero:** `current_hashrate == 0.0f` por **`60 s` consecutivos**.
    2. **Domínio Assimétrico Congelado:** qualquer um dos domínios do `BM1370` (`domain_measurements[0][0..3]`) apresentar hashrate **`< 10%` da média dos demais domínios por `45 s` consecutivos**.
  - Execução síncrona da recuperação em 3 níveis dentro da `POWER_MANAGEMENT_task` (`main/tasks/power_management_task.c`):
    1. **Nível 1 (1ª falha ou após janela `>= 10 min` sem falhas):** `mining_stop()` → aguarda `2 s` → `mining_start()`, preservando Wi-Fi/Stratum e estatísticas, e incrementando `watchdog_resets`.
    2. **Nível 2 (2ª e 3ª falhas em janela `< 10 min`):** aplica *step-down* preventivo de **`-12.5 MHz`** na frequência ativa em RAM (respeitando o piso de `400 MHz`, sem sobrescrever a NVS) antes de executar `mining_stop()` → aguarda `2 s` → `mining_start()`, incrementando `watchdog_resets`.
    3. **Nível 3 (4ª falha consecutiva em janela `< 10 min`):** emite log crítico e aciona `esp_restart()`.

## Capabilities

### New Capabilities
- `i2c-and-tps546-resilience`: Redução de tráfego PMBus/I2C no regulador `TPS546D24A`, tratamento não-fatal de erros de comunicação `STATUS_CML` e recuperação automática de barramento I2C (`i2c_master_bus_reset`).
- `asic-hashrate-watchdog`: Detecção de travamento global (`0 GH/s` por `60 s`) ou congelamento assimétrico de domínio (`< 10%` por `45 s`) no ASIC e recuperação autônoma escalonada em 3 níveis na `POWER_MANAGEMENT_task`.

### Modified Capabilities
<!-- Nenhuma capability existente em openspec/specs/ tem requisitos alterados -->

## Impact

- **Arquivos afetados:**
  - `main/power/TPS546.h`, `main/power/TPS546.c`, `main/power/vcore.c`
  - `main/i2c_bitaxe.c`
  - `main/tasks/power_management_task.h`, `main/tasks/power_management_task.c`
  - `main/tasks/hashrate_monitor_task.c`
  - `doc/FIRMWARE_IMPROVEMENTS.md`
- **Impacto Operacional:** Reduz em ~43% o tráfego I2C periódico no `TPS546D24A`, elimina interrupções por `STATUS_CML=0x02 COMM` e adiciona autorrecuperação sem perda de sessão Stratum para travamentos do `BM1370`.
