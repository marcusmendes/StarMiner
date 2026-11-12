# Tasks: phase-2-i2c-tps546-and-asic-watchdog

## 1. Fase 2.1 — Otimização I2C, Tratamento `STATUS_CML` e Bus Reset (`i2c-and-tps546-resilience`)

- [x] 1.1 Atualizar `main/tasks/power_management_task.h` adicionando os campos de telemetria e coordenação da Fase 2 (`cml_error_count`, `watchdog_resets`, `asic_watchdog_trigger`, `watchdog_consecutive_failures`, `watchdog_last_failure_us`, `last_freq_change_us`, `watchdog_freq_offset_mhz`) em `PowerManagementModule`, verificando a compilação dos módulos que incluem o header.
- [x] 1.2 Atualizar `main/power/TPS546.c` para: (a) armazenar `PMBUS_VOUT_MODE` (`0x20`) em cache estático (`s_vout_mode` / `s_vout_mode_cached`) durante `TPS546_init()` e reutilizá-lo em `ulinear16_2_float()` e `float_2_ulinear16()` sem leituras I2C; (b) deduplicar leituras consecutivas de `TPS546_get_vout()` dentro da janela do mesmo ciclo de `100 ms` (`< 50 ms`, invalidando em `TPS546_set_vout()`), verificando a compilação sem warnings.
- [x] 1.3 Atualizar `TPS546_check_status()` em `main/power/TPS546.c` para inspecionar `status & TPS546_STATUS_CML` quando não houver falha elétrica/térmica crítica ativa, lendo `PMBUS_STATUS_CML` (`0x7E`), incrementando `GLOBAL_STATE->POWER_MANAGEMENT_MODULE.cml_error_count`, emitindo `ESP_LOGW` e enviando `TPS546_clear_faults()` (`PMBUS_CLEAR_FAULTS`) sem acionar `power_fault = 1`, verificando a compilação.
- [x] 1.4 Atualizar `i2c_transfer_with_retries()` em `main/i2c_bitaxe.c` para invocar `i2c_master_bus_reset(i2c_bus_handle)` antes do delay de espera quando uma tentativa de transferência falhar antes da última tentativa (`i + 1 < I2C_RETRY_COUNT`), verificando a compilação limpa do módulo I2C.

## 2. Fase 2.2 — Watchdog de `0 GH/s` e Domínios Congelados em 3 Níveis (`asic-hashrate-watchdog`)

- [x] 2.1 Atualizar `main/tasks/hashrate_monitor_task.c` para monitorar continuamente (quando `ASIC_initalized == true`, `SYSTEM_MODULE.work_received > 0`, `!pools_unavailable`, `!mining_paused`, `overheat_mode == 0`, `power_fault == 0`, `!hardware_fault`, `!SELF_TEST_MODULE.is_active` e após `30 s` de carência desde `last_freq_change_us`) as condições de: (a) `current_hashrate == 0.0f` por `60 s` consecutivos; e (b) qualquer domínio de hash `< 10%` da média dos demais domínios por `45 s` consecutivos, sinalizando `POWER_MANAGEMENT_MODULE.asic_watchdog_trigger = true`, verificando a compilação.
- [x] 2.2 Atualizar `main/tasks/power_management_task.c` para registrar `last_freq_change_us` nas inicializações/mudanças de frequência e processar sincronamente `asic_watchdog_trigger == true` em 3 níveis (Nível 1: `mining_stop()` → `2 s` → `mining_start()` + `watchdog_resets++`; Nível 2 na 2ª/3ª falha em `< 10 min`: step-down em RAM `watchdog_freq_offset_mhz += 12.5f` até o piso de `400 MHz` sem gravar na NVS antes do re-init; Nível 3 na 4ª falha consecutiva em `< 10 min`: `esp_restart()`), verificando a compilação.

## 3. Verificação Integrada de Build e Documentação

- [x] 3.1 Executar `idf.py build` no ambiente ESP-IDF v5.5.x e confirmar que o firmware compila sem erros ou warnings.
- [x] 3.2 Atualizar o checklist da Fase 2 em `doc/FIRMWARE_IMPROVEMENTS.md` marcando os itens implementados.
