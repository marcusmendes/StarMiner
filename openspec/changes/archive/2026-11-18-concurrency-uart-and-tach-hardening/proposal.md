# Proposal

## Why

Mesmo após o isolamento de núcleos (*Core Pinning*) e as correções iniciais de concorrência da Fase 1, quatro vulnerabilidades sutis herdadas do código-base `ESP-Miner` ainda expõem as placas Bitaxe Gamma (`BM1370` / `601a-4`) a pânicos, condições de corrida na `UART1` e *Use-After-Free*:
1. `EMC2101_get_fan_speed()` (`main/thermal/EMC2101.c:139`) divide `5400000 / reading` sem verificar `reading == 0`, podendo causar exceção fatal de divisão inteira por zero (`ESP_RST_PANIC`) em caso de glitch I2C.
2. `mining_stop()` (`main/tasks/power_management_task.c:172-193`) marca `GLOBAL_STATE->ASIC_initalized = false` apenas *após* desacelerar o PLL para `50 MHz`, cortar o `VCore` para `0 V` e baixar `RST`, permitindo que `create_jobs_task` e `hashrate_monitor_task` no Core 1 continuem enviando pacotes na `UART1` durante o desligamento do ASIC.
3. `ASIC_result_task()` (`main/tasks/asic_result_task.c:46-136`) e `SYSTEM_notify_found_nonce()` (`main/system.c:351`) leem `active_jobs[job_id]` (e seus campos `jobid`, `extranonce2` e `target`) *após* liberar `valid_jobs_lock` e atravessar escrita TCP potencialmente bloqueante (`STRATUM_V1_submit_share`), criando janela de *Use-After-Free* caso o anel de 16 slots do `BM1370` dê a volta.
4. `system_api_add_hashrate_monitor()` (`main/http_server/system_api_json.c:231-256`) serializa `total_measurement`, `error_measurement` e `domain_measurements` sem adquirir `HASHRATE_MONITOR_MODULE.lock`.

## What Changes

- **Guarda contra divisão por zero na leitura do tacômetro (`main/thermal/EMC2101.c`)**:
  - Verificar `reading == 0` e `reading == 0xFFFF` em `EMC2101_get_fan_speed()` antes de calcular `5400000 / reading`, retornando `0 RPM` com segurança.
- **Silenciamento prévio do Core 1 antes do desligamento do ASIC (`main/tasks/power_management_task.c`)**:
  - Atribuir `GLOBAL_STATE->ASIC_initalized = false` no início de `mining_stop()`, aguardar a drenagem das operações em curso na `UART1` e somente então executar a rampa para `50 MHz`, o corte de `VCore` e o `RST LOW`.
- **Snapshot imutável de `bm_job` sob `valid_jobs_lock` (`main/tasks/asic_result_task.c`, `main/system.c`)**:
  - Copiar por valor os metadados necessários de `active_jobs[job_id]` (incluindo buffers locais de `jobid`, `extranonce2` e `target`) enquanto `valid_jobs_lock` está retido, eliminando qualquer leitura de ponteiro compartilhado após a liberação do mutex ou após I/O de rede.
- **Serialização sincronizada de `hashrateMonitor` (`main/http_server/system_api_json.c`)**:
  - Envolver a leitura dos contadores por ASIC e por domínio em `system_api_add_hashrate_monitor()` com `HASHRATE_MONITOR_MODULE.lock`.

## Capabilities

### New Capabilities
<!-- Nenhuma capability nova; todas as correções fortalecem contratos existentes. -->

### Modified Capabilities
- `job-pipeline-concurrency`: Adicionar requisitos de snapshot imutável de `bm_job` sob `valid_jobs_lock`, silenciamento prévio de `ASIC_initalized` em `mining_stop()` e serialização protegida por `HASHRATE_MONITOR_MODULE.lock` em `system_api_add_hashrate_monitor()`.
- `i2c-and-tps546-resilience`: Adicionar requisito de proteção contra divisão por zero e leituras inválidas (`0x0000` / `0xFFFF`) na leitura de tacômetro do `EMC2101`.

## Impact

- **Código Afetado**:
  - `main/thermal/EMC2101.c` (`EMC2101_get_fan_speed`)
  - `main/tasks/power_management_task.c` (`mining_stop`)
  - `main/tasks/asic_result_task.c` (`ASIC_result_task`)
  - `main/system.c` / `main/system.h` (`SYSTEM_notify_found_nonce` ou leitura protegida de `target`)
  - `main/http_server/system_api_json.c` (`system_api_add_hashrate_monitor`)
- **APIs e Contratos**:
  - Nenhuma alteração de contrato HTTP/JSON ou NVS; apenas endurecimento interno de concorrência e segurança contra *panic*.
