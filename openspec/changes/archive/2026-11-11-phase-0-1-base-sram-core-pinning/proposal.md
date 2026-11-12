# Proposal

## Why

A placa StarMiner Bitaxe Gamma (`GammaV2` / `601a-4`, modelo NVS `"LT-001"`, DC 12 V, BM1370) opera hoje com SRAM interna criticamente baixa (`~22,9 kB` livres), falha de fallback na identificação da família `GammaV2` / sub-revisão `601a-4` / alias `LT-001` no código-base público e condições de corrida (*Use-After-Free* e reset indevido de hashrate) no pipeline de jobs entre Stratum e ASIC. Estabilizar essa base (Fases 0 e 1 de `doc/FIRMWARE_IMPROVEMENTS.md`) é pré-requisito obrigatório antes de evoluir o controle térmico/I2C (Fases 2 e 3) e o Auto-Tuner DVFS (Fase 4).

## What Changes

- **Compatibilidade e Fallback Resiliente para `601a-4` / `LT-001` (Fase 0):**
  - Inclusão de `FAMILY_GAMMA_V2` e `FAMILY_GAMMA_DUO` em `default_families[]` e entrada explícita `"601a-4"` em `default_configs[]` (`main/device_config.h`).
  - Fallback por prefixo `"601a*"` (herdando os parâmetros elétricos/térmicos da `601a` e preservando a string original de `board_version`) e tratamento do alias `devicemodel = "LT-001"` como `FAMILY_GAMMA_V2` em `device_config_init` (`main/device_config.c`).
- **Alívio de SRAM Interna para PSRAM e Isolamento de Núcleos — *Core Pinning* (Fase 1.1):**
  - Migração das stacks das tasks elegíveis (`POWER_MANAGEMENT_task`, `FAN_CONTROLLER_task`, `create_jobs_task`, `ASIC_result_task`, `hashrate_monitor_task`, `statistics_task`, `protocol_coordinator_task`, `stratum_v1_task`, `stratum_v2_task`, `websocket_log_task`, `websocket_api_task` e tasks `BAP`) para PSRAM quando `esp_psram_is_initialized()` for verdadeiro (com fallback seguro para SRAM interna).
  - Preservação estrita de `nvs_task` e da task do servidor `httpd` em SRAM interna (Core 0) para segurança durante operações de escrita em Flash SPI / OTA.
  - Migração dos buffers de heap (`rest_server_context_t`, `scratch_buf` do WebSocket, buffer RX do BAP, structs/strings `bm_job` e `json_rpc_buffer` / `mining_notify` do Stratum V1) para PSRAM com verificação de `esp_psram_is_initialized()`.
  - Aplicação de *Core Pinning*: **Core 1** dedicado ao tempo real de mineração (`ASIC_result_task` prio 15, `create_jobs_task` prio 10, `hashrate_monitor_task` prio 5) e **Core 0** dedicado a sistema, Wi-Fi/LwIP, `httpd`, Stratum, Power/Fan, estatísticas, NVS e BAP.
- **Correções de Thread-Safety e Concorrência no Pipeline de Jobs (Fase 1.2):**
  - Execução de `decode_mining_notification` antes de `queue_enqueue` em `stratum_v1_task.c` para eliminar *Use-After-Free* entre Core 0 e Core 1.
  - Proteção integral de `active_jobs[job_id]` e `valid_jobs[job_id]` sob `valid_jobs_lock` em `BM1370_send_work` e `BM1370_process_work` (`components/asic/bm1370.c`).
  - Limpeza completa dos 128 índices de `valid_jobs` sob `valid_jobs_lock` e remoção de `hashrate_monitor_reset_measurements` em `SYSTEM_clean_jobs_queue` (`main/system.c`).
  - Cálculo único do deadline absoluto `timeout_time` antes do loop `while (queue->count == 0)` e proteção contra `timeout_ms <= 0` em `queue_dequeue_timeout` (`main/work_queue.c`).
- **Fora de Escopo (Próximas Changes):**
  - Fase 2 (Otimização I2C `TPS546D24A`, tratamento de `STATUS_CML` e Watchdog de 3 níveis).
  - Fase 3 (*Soft-Throttling* térmico em RAM e proteção de emergência sem escrita na NVS).
  - Fase 4 (Compensação de *Vdroop*, perfis DVFS e novos campos na API/UI `AxeOS`).

## Capabilities

### New Capabilities
- `device-config`: Reconhecimento de hardware, mapeamento de família (`FAMILY_GAMMA_V2` / `FAMILY_GAMMA_DUO`) e fallback resiliente para revisões `601a*` e modelo `LT-001`.
- `memory-and-core-pinning`: Gerenciamento de alocação de stacks e buffers em PSRAM (preservando tasks críticas de Flash em SRAM interna) e isolamento determinístico de tasks entre Core 0 e Core 1.
- `job-pipeline-concurrency`: Segurança de concorrência (*thread-safety*), sincronização de fila com timeout absoluto e ciclo de vida de `mining_notify` / `bm_job` entre Stratum e ASIC.

### Modified Capabilities
<!-- Nenhuma capability existente em openspec/specs/ -->

## Impact

- **Arquivos afetados:**
  - `main/device_config.h`, `main/device_config.c`
  - `main/main.c`, `main/tasks/protocol_coordinator.c`
  - `main/http_server/http_server.c`, `main/http_server/websocket_log.c`
  - `main/bap/bap_uart.c`, `main/bap/bap_subscription.c`
  - `main/tasks/create_jobs_task.c`, `components/stratum/stratum_api.c`
  - `main/tasks/stratum_v1_task.c`, `components/asic/bm1370.c`, `main/system.c`, `main/work_queue.c`
- **APIs / Hardware:** Mantém compatibilidade binária com a partição NVS original da placa StarMiner (`601a` / `601a-4` / `LT-001`), elevando a SRAM interna livre de `~22,9 kB` para `> 85 kB` e eliminando condições de corrida entre os núcleos do ESP32-S3.
