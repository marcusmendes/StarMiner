# Design: phase-2-i2c-tps546-and-asic-watchdog

## Context

Consulte `proposal.md` para motivação e escopo. No código atual:
1. Em `main/power/TPS546.c`, `ulinear16_2_float()` e `float_2_ulinear16()` executam `smb_read_byte(PMBUS_VOUT_MODE, &voutmode)` a cada invocação. Além disso, em `main/tasks/power_management_task.c` (linhas 138–139), `Power_get_output()` e `VCORE_get_voltage_mv()` chamam `TPS546_get_vout()` consecutivamente a cada `100 ms`, gerando 4 transações I2C apenas para ler `VOUT` uma vez por ciclo (40 transações/s somadas a `VIN`, `IOUT`, `TEMP` e `STATUS_WORD`).
2. `TPS546_check_status()` ignora `TPS546_STATUS_CML` quando ocorre isoladamente, deixando o bit `CML` travado no `STATUS_WORD` sem limpar a falha (`PMBUS_CLEAR_FAULTS`) e sem contabilizar a telemetria (`cml_error_count`).
3. Em `main/i2c_bitaxe.c`, `i2c_transfer_with_retries()` apenas aguarda `10 ms` entre falhas sem invocar `i2c_master_bus_reset(i2c_bus_handle)`.
4. Em `main/tasks/hashrate_monitor_task.c`, o loop executa a cada `POLL_RATE = 1000 ms` (`1 s`), mas não monitora condição de `0 GH/s` prolongado nem congelamento assimétrico de um dos 4 domínios do `BM1370`.

## Goals / Non-Goals

**Goals:**
- Eliminar 100% das leituras I2C de `PMBUS_VOUT_MODE` após o `TPS546_init()` e eliminar a leitura duplicada de `PMBUS_READ_VOUT` no ciclo de `100 ms`.
- Tratar `TPS546_STATUS_CML` de forma não-fatal com contagem em `POWER_MANAGEMENT_MODULE.cml_error_count` e limpeza via `PMBUS_CLEAR_FAULTS`.
- Destravar o barramento `I2C0` via `i2c_master_bus_reset()` nos retries de `i2c_transfer_with_retries()`.
- Detectar `0 GH/s` (`60 s`) e domínio congelado (`< 10%` por `45 s`) em `hashrate_monitor_task` (Core 1) e executar recuperação síncrona de 3 níveis na `POWER_MANAGEMENT_task` (Core 0) sem tocar na NVS.

**Non-Goals:**
- Implementar a máquina de estados de *Soft-Throttling* térmico (`>= 68 °C` ASIC / `>= 90 °C` VR) — reservada para a Fase 3.
- Expor `cmlErrorCount` e `watchdogResets` no `openapi.yaml` / `AxeOS` — reservado para a Fase 4.

## Decisions

### 1. Cache de `PMBUS_VOUT_MODE` e deduplicação temporal de `TPS546_get_vout()`
- **Decisão:**
  - Em `main/power/TPS546.c`, armazenar `static uint8_t s_vout_mode = 0x97;` e `static bool s_vout_mode_cached = false;`. Durante `TPS546_init()`, quando `smb_read_byte(PMBUS_VOUT_MODE, &voutmode)` tiver sucesso, gravar `s_vout_mode = voutmode; s_vout_mode_cached = true;`. Em `ulinear16_2_float()` e `float_2_ulinear16()`, usar `s_vout_mode` diretamente (fazendo leitura I2C apenas se `!s_vout_mode_cached`).
  - Em `TPS546_get_vout()`, registrar o timestamp da última leitura bem-sucedida (`s_last_vout_read_us = esp_timer_get_time()`). Se `TPS546_get_vout()` for chamado novamente dentro de `< 50 ms` (`50000 us`) e `last_vout > 0.0f`, retornar `last_vout` imediatamente sem nova transação I2C. Invalidar `s_last_vout_read_us = 0` em `TPS546_set_vout()`.
- **Racional:** Como `Power_get_output()` e `VCORE_get_voltage_mv()` são executados em linhas consecutivas do loop de `100 ms` da `POWER_MANAGEMENT_task`, a janela de `50 ms` elimina a leitura duplicada mantendo total transparência para chamadas avulsas ou mudanças de tensão via `TPS546_set_vout()`.

### 2. Tratamento não-fatal de `TPS546_STATUS_CML` em `TPS546_check_status()`
- **Decisão:**
  - Verificar primeiro as falhas críticas (`TPS546_STATUS_OFF | TPS546_STATUS_VOUT_OV | TPS546_STATUS_IOUT_OC | TPS546_STATUS_VIN_UV | TPS546_STATUS_TEMP`).
  - No ramo `else` (sem falha crítica, `SYSTEM_MODULE->power_fault = 0`), verificar `if (status & TPS546_STATUS_CML)`:
    - Ler `PMBUS_STATUS_CML` (`0x7E`) via `smb_read_byte`.
    - Incrementar `GLOBAL_STATE->POWER_MANAGEMENT_MODULE.cml_error_count`.
    - Registrar `ESP_LOGW(TAG, "TPS546 non-fatal STATUS_CML=0x%02X (word=0x%04X, count=%lu) - clearing fault", cml_val, status, ...)`.
    - Chamar `TPS546_clear_faults()` (`PMBUS_CLEAR_FAULTS`) e retornar `ESP_OK`.

### 3. Arquitetura do Watchdog de ASIC entre Core 1 (`hashrate_monitor_task`) e Core 0 (`POWER_MANAGEMENT_task`)
- **Decisão:**
  ```text
  +--------------------------------------+       +----------------------------------------+
  | Core 1: hashrate_monitor_task (1 s)  |       | Core 0: POWER_MANAGEMENT_task (100 ms) |
  +--------------------------------------+       +----------------------------------------+
  | 1. Verifica armacao + carencia 30 s  |       | 1. Detecta asic_watchdog_trigger==true |
  |    (pos-init ou mudanca de clock)    |       | 2. Avalia janela de 10 min:            |
  | 2. Acumula tempo (dt_us):            |       |    - 1a falha (Nivel 1):               |
  |    * total == 0 GH/s >= 60 s         |       |      mining_stop -> 2s -> mining_start |
  |    * dominio < 10% media outros >=45s|       |    - 2a/3a falha < 10 min (Nivel 2):   |
  | 3. Seta asic_watchdog_trigger = true +------>|      watchdog_freq_offset += 12.5 MHz  |
  |    e zera contadores                 |       |      (piso 400 MHz em RAM, sem NVS)    |
  +--------------------------------------+       |      mining_stop -> 2s -> mining_start |
                                                 |    - 4a falha < 10 min (Nivel 3):      |
                                                 |      ESP_LOGE + esp_restart()          |
                                                 +----------------------------------------+
  ```
  - **Campos adicionados em `PowerManagementModule` (`main/tasks/power_management_task.h`):**
    - `uint32_t cml_error_count;`
    - `uint32_t watchdog_resets;`
    - `volatile bool asic_watchdog_trigger;`
    - `uint8_t watchdog_consecutive_failures;`
    - `int64_t watchdog_last_failure_us;`
    - `int64_t last_freq_change_us;`
    - `float watchdog_freq_offset_mhz;`
  - **Preservação do step-down em RAM sem gravar na NVS:**
    - `POWER_MANAGEMENT_init_frequency()` calcula a frequência alvo efetiva como `fmaxf(400.0f, nvs_freq - power_management->watchdog_freq_offset_mhz)` (quando `nvs_freq >= 400.0f`) e atualiza `power_management->last_freq_change_us = esp_timer_get_time()`.
    - No loop principal de `POWER_MANAGEMENT_task`, se o usuário alterar `NVS_CONFIG_ASIC_FREQUENCY` (`nvs_freq != last_nvs_asic_frequency`), `watchdog_freq_offset_mhz` e `watchdog_consecutive_failures` são zerados para respeitar o novo alvo do usuário.

## Risks / Trade-offs

- **[Risco] Falso positivo do Watchdog durante rampa de frequência ou reconexão de pool** → **Mitigação:** A condição de armação exige `ASIC_initalized == true`, `SYSTEM_MODULE.work_received > 0`, `!pools_unavailable`, `!mining_paused`, `overheat_mode == 0`, `power_fault == 0` e `esp_timer_get_time() - last_freq_change_us >= 30 * 1000000LL` (`30 s` de carência após qualquer boot, recovery ou mudança de frequência).
- **[Risco] Reset de barramento I2C durante transação de outro dispositivo** → **Mitigação:** Todas as chamadas ao `I2C0` já utilizam o mesmo `i2c_bus_handle` gerenciado pelo driver `i2c_master` do ESP-IDF v5.5, e o `i2c_master_bus_reset` só é acionado quando uma transação já falhou por timeout/NACK antes da última tentativa.
