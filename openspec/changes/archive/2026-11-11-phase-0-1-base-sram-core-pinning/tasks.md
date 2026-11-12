# Tasks: phase-0-1-base-sram-core-pinning

## 1. Fase 0 — Compatibilidade `601a-4` / `LT-001` e Fallback Resiliente (`device-config`)

- [x] 1.1 Confirmar a existência e integridade dos arquivos de backup físico em `bitaxe-backup/` (`flash_16MB_601a-4_V1.0.1-rc.10.bin` com `16.777.216` bytes e `nvs_601a-4_original.bin` com `24.576` bytes).
- [x] 1.2 Atualizar `main/device_config.h` adicionando `FAMILY_GAMMA_V2` e `FAMILY_GAMMA_DUO` em `default_families[]` e a entrada explícita `"601a-4"` em `default_configs[]` (espelhando `"601a"`), verificando a compilação do módulo `main`.
- [x] 1.3 Atualizar `main/device_config.c` (`device_config_init`) para incluir fallback por prefixo `"601a"` (`strncmp(board_version, "601a", 4) == 0`, preservando `board_version` com `strdup`) e mapear o alias `devicemodel = "LT-001"` para `FAMILY_GAMMA_V2`, verificando a compilação sem warnings.

## 2. Fase 1.1 — Alívio de SRAM Interna (PSRAM) e Isolamento de Núcleos (`memory-and-core-pinning`)

- [x] 2.1 Atualizar `main/main.c` e `main/tasks/protocol_coordinator.c` para criar as tasks elegíveis com `xTaskCreatePinnedToCoreWithCaps` (verificando `esp_psram_is_initialized()` para usar `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` com fallback para `MALLOC_CAP_DEFAULT`), fixando `ASIC_result_task` (prio 15), `create_jobs_task` (prio 10) e `hashrate_monitor_task` (prio 5) no **Core 1**, e `POWER_MANAGEMENT_task` (prio 10), `FAN_CONTROLLER_task` (prio 5), `statistics_task` (prio 3), `protocol_coordinator_task`, `stratum_v1_task`, `stratum_v2_task`, `self_test_task` e monitores no **Core 0**, verificando que `nvs_task` (`main/nvs_config.c`) permanece em SRAM interna.
- [x] 2.2 Atualizar `main/http_server/http_server.c` e `main/http_server/websocket_log.c` para: (a) fixar `httpd` no Core 0 (`config.core_id = 0`) mantendo sua stack em SRAM interna; (b) fixar `websocket_log_task` e `websocket_api_task` no Core 0 em PSRAM; (c) alocar `rest_server_context_t` (`10,5 KB`) e `scratch_buf` (`4 KB`) em PSRAM quando `esp_psram_is_initialized()` for verdadeiro, verificando a compilação limpa do servidor HTTP.
- [x] 2.3 Atualizar `main/bap/bap_uart.c` e `main/bap/bap_subscription.c` para fixar as tasks BAP no Core 0 com `xTaskCreatePinnedToCoreWithCaps` e alocar o buffer RX `data` (`BAP_BUF_SIZE`) em PSRAM com verificação de `esp_psram_is_initialized()`, validando a compilação do subsistema BAP.
- [x] 2.4 Atualizar `main/tasks/create_jobs_task.c` (`generate_work`, `generate_work_sv2`, `generate_work_sv2_ext`) e `components/stratum/stratum_api.c` (`STRATUM_V1_initialize_buffer`, `realloc_json_buffer`, `STRATUM_V1_parse`) para alocar `bm_job`, `jobid`, `extranonce2`, `json_rpc_buffer` e `mining_notify` (incluindo strings e `merkle_branches`) em PSRAM quando `esp_psram_is_initialized()` for verdadeiro, verificando a compilação dos componentes `stratum` e `main`.

## 3. Fase 1.2 — Thread-Safety e Concorrência no Pipeline de Jobs (`job-pipeline-concurrency`)

- [x] 3.1 Atualizar `main/tasks/stratum_v1_task.c` para executar `decode_mining_notification(GLOBAL_STATE, stratum_api_v1_message.mining_notification)` **antes** de `queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification)`, eliminando o *Use-After-Free* entre Core 0 e Core 1 e verificando a compilação.
- [x] 3.2 Atualizar `components/asic/bm1370.c` em `BM1370_send_work` e `BM1370_process_work` para proteger `free_bm_job(active_jobs[job.job_id])`, a atribuição de `active_jobs[job.job_id]`, a verificação de `valid_jobs[job_id]` e a leitura de `active_jobs[job_id]->version` integralmente sob `pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock)`, verificando a compilação do componente `asic`.
- [x] 3.3 Atualizar `main/system.c` (`SYSTEM_clean_jobs_queue`) para zerar todos os 128 índices de `GLOBAL_STATE->valid_jobs` (`memset`) sob `valid_jobs_lock` e remover a chamada a `hashrate_monitor_reset_measurements(GLOBAL_STATE)`, verificando a compilação.
- [x] 3.4 Atualizar `main/work_queue.c` (`queue_dequeue_timeout`) para tratar `timeout_ms <= 0` com segurança e calcular o deadline absoluto `timeout_time` uma única vez antes do laço `while (queue->count == 0)`, verificando a compilação.

## 4. Verificação Integrada de Build e Testes

- [x] 4.1 Executar `idf.py build` no ambiente ESP-IDF v5.5.x e verificar que o firmware completo e os binários são gerados sem erros ou warnings introduzidos pelas alterações da Fase 0 e Fase 1.
- [x] 4.2 Atualizar o checklist das Fases 0 e 1 em `doc/FIRMWARE_IMPROVEMENTS.md` refletindo a conclusão das tarefas implementadas.
