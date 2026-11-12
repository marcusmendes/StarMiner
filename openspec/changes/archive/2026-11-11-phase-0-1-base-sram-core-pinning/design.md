# Design: phase-0-1-base-sram-core-pinning

## Context

Consulte `proposal.md` para motivação e escopo geral. No estado atual do repositório (`master` em `3bf9c3e`):
1. A placa física StarMiner Bitaxe Gamma (`601a-4`) traz `boardversion = "601a"` e `devicemodel = "LT-001"` na partição NVS original (`bitaxe-backup/nvs_601a-4_original.bin`), enquanto a build de fábrica (`V1.0.1-rc.10`) reporta `"601a-4"` via leitura de sub-revisão em `GPIO6/ADC1_CH5`. No código público, `default_families[]` omite `FAMILY_GAMMA_V2` e `FAMILY_GAMMA_DUO`, e `default_configs[]` não possui `"601a-4"`, deixando qualquer revisão `"601a-*"` gravada na NVS vulnerável a falha em `VCORE_init` ou configuração errônea de 5 V no `TPS546D24A`.
2. A SRAM interna livre em operação medida na placa é de apenas `22,9 kB` (com `7,95 MB` livres na PSRAM). Tasks como `POWER_MANAGEMENT_task`, `FAN_CONTROLLER_task`, `create_jobs_task` e `ASIC_result_task` ainda alocam stacks na SRAM interna e todas as tasks de aplicação são criadas sem afinidade de núcleo (`tskNO_AFFINITY`).
3. O pipeline de mineração apresenta três corridas críticas quando executado em paralelo entre Core 0 e Core 1: leitura pós-enfileiramento de `mining_notification` em `stratum_v1_task.c`, acesso desprotegido a `active_jobs[job_id]` em `bm1370.c` e limpeza parcial (`i += 4`) com reset indevido de hashrate em `SYSTEM_clean_jobs_queue`.

## Goals / Non-Goals

**Goals:**
- Garantir que `device_config_init` resolva deterministicamente `"601a"`, `"601a-4"`, qualquer prefixo `"601a*"` e o alias `devicemodel = "LT-001"` para `FAMILY_GAMMA_V2` (12 V, `TPS546`, `EMC2101`).
- Liberar `~60–90 KB` de SRAM interna migrando stacks e buffers elegíveis para PSRAM com verificação de `esp_psram_is_initialized()`, mantendo `nvs_task` e `httpd` em SRAM interna.
- Isolar o tempo real de mineração no **Core 1** e o restante do sistema no **Core 0** via `xTaskCreatePinnedToCoreWithCaps`.
- Eliminar todas as condições de corrida em `stratum_v1_task.c`, `bm1370.c`, `system.c` e `work_queue.c`.

**Non-Goals:**
- Alterar o tráfego I2C do `TPS546D24A` ou implementar o Watchdog de 3 níveis (reservados para a Fase 2).
- Modificar a máquina de estados térmica ou implementar *Soft-Throttling* / *Vdroop* / DVFS (reservados para as Fases 3 e 4).
- Alterar contratos da API HTTP ou o frontend `AxeOS` nesta fase.

## Decisions

### 1. Resolução em cascata em `device_config_init` (`main/device_config.h` e `main/device_config.c`)
- **Decisão:**
  1. Adicionar `FAMILY_GAMMA_V2` e `FAMILY_GAMMA_DUO` em `default_families[]`.
  2. Adicionar `"601a-4"` explicitamente em `default_configs[]` com a mesma configuração de `"601a"`.
  3. Em `device_config_init`, após o loop de busca exata em `default_configs[]`, adicionar um fallback por prefixo `strncmp(board_version, "601a", 4) == 0` que copia a entrada `"601a"` de `default_configs[]` e sobrescreve `GLOBAL_STATE->DEVICE_CONFIG.board_version = strdup(board_version)` para preservar a revisão específica.
  4. No fallback de `default_families[]`, considerar `strcasecmp(device_model, "LT-001") == 0` como match para `FAMILY_GAMMA_V2`.
- **Alternativas consideradas:** Apenas cadastrar `"601a-4"` em `default_configs[]`. Rejeitada porque futuras revisões (`"601a-5"`) ou configurações customizadas com `devicemodel = "LT-001"` voltariam a falhar.

### 2. Helper padronizado de alocação e criação de tasks em PSRAM com *Core Pinning*
- **Decisão:**
  - Sempre verificar `esp_psram_is_initialized()`:
    - Para buffers: `esp_psram_is_initialized() ? heap_caps_malloc(sz, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : malloc(sz)` (e análogo com `heap_caps_calloc` / `strdup` em PSRAM).
    - Para tasks: `xTaskCreatePinnedToCoreWithCaps(..., core_id, ..., esp_psram_is_initialized() ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_DEFAULT)`.
  - **Mapa de Core Pinning e Prioridades:**
    ```text
    +-----------------------------------+--------+------------+------------------+
    | Task                              | Nucleo | Prioridade | Memoria da Stack |
    +-----------------------------------+--------+------------+------------------+
    | ASIC_result_task                  | Core 1 |     15     | PSRAM (8192 B)   |
    | create_jobs_task                  | Core 1 |     10     | PSRAM (8192 B)   |
    | hashrate_monitor_task             | Core 1 |      5     | PSRAM (8192 B)   |
    +-----------------------------------+--------+------------+------------------+
    | POWER_MANAGEMENT_task             | Core 0 |     10     | PSRAM (8192 B)   |
    | self_test_task (quando ativo)     | Core 0 |     10     | PSRAM (8192 B)   |
    | FAN_CONTROLLER_task               | Core 0 |      5     | PSRAM (8192 B)   |
    | protocol_coordinator_task         | Core 0 |      5     | PSRAM (3072 B)   |
    | stratum_v1_task / stratum_v2_task | Core 0 |      5     | PSRAM (8K / 12K) |
    | BAP UART / Subscription tasks     | Core 0 |      5     | PSRAM (4K / 8K)  |
    | statistics_task                   | Core 0 |      3     | PSRAM (8192 B)   |
    | websocket_log / websocket_api     | Core 0 |      2     | PSRAM (8192 B)   |
    | cpu_monitor / task_monitor        | Core 0 |      1     | PSRAM (4K / 8K)  |
    | nvs_task                          | Core 0 |      5     | SRAM Interna     |
    | httpd (config.core_id = 0)        | Core 0 |    padrao  | SRAM Interna     |
    +-----------------------------------+--------+------------+------------------+
    ```
- **Racional:** No código atual (`main/main.c`), `create_jobs_task` tinha prioridade `20` (acima de `ASIC_result_task` com `15`). Inverter para `ASIC_result_task` (`15`) > `create_jobs_task` (`10`) no Core 1 garante que a recepção de nonces/registradores via UART1 nunca sofra atraso ou *FIFO overflow* enquanto um novo cabeçalho Merkle está sendo calculado. Manter `nvs_task` e `httpd` em SRAM interna respeita a restrição do ESP-IDF para tasks que invocam operações de escrita/apagamento na Flash SPI (`esp_ota_write`, `nvs_commit`).

### 3. Invariantes de Sincronização no Pipeline de Jobs
- **Decisão:**
  1. **`stratum_v1_task.c`:** Chamar `decode_mining_notification(GLOBAL_STATE, stratum_api_v1_message.mining_notification)` **antes** de `queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification)`. Como `queue_enqueue` sinaliza `not_empty`, o ponteiro passa a pertencer exclusivamente ao consumidor (`create_jobs_task` no Core 1) a partir do momento em que entra na fila.
  2. **`components/asic/bm1370.c`:**
     - Em `BM1370_send_work`: adquirir `pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock)` **antes** de verificar/liberar `GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job.job_id]`, atualizar `active_jobs[job.job_id] = next_bm_job`, marcar `GLOBAL_STATE->valid_jobs[job.job_id] = 1` e liberar o mutex.
     - Em `BM1370_process_work`: adquirir `pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock)` antes de inspecionar `GLOBAL_STATE->valid_jobs[job_id]` e `GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]`. Se `valid_jobs[job_id] == 0` ou `active_jobs[job_id] == NULL`, desbloquear o mutex e retornar `NULL`. Caso contrário, ler `uint32_t base_version = GLOBAL_STATE->ASIC_TASK_MODULE.active_jobs[job_id]->version` dentro do lock e liberar o mutex imediatamente.
  3. **`main/system.c` (`SYSTEM_clean_jobs_queue`):** Substituir o loop `for (int i = 0; i < 128; i = i + 4)` por `memset(GLOBAL_STATE->valid_jobs, 0, sizeof(GLOBAL_STATE->valid_jobs))` dentro de `valid_jobs_lock` e remover a chamada a `hashrate_monitor_reset_measurements(GLOBAL_STATE)`.
  4. **`main/work_queue.c` (`queue_dequeue_timeout`):** Se `timeout_ms <= 0` e `queue->count == 0`, desbloquear o mutex e retornar `NULL` imediatamente. Caso contrário, calcular `struct timespec timeout_time` uma única vez antes do `while (queue->count == 0)`.

## Risks / Trade-offs

- **[Risco] Crash por acesso à PSRAM durante escrita em Flash SPI (`Cache disabled`)** → **Mitigação:** `nvs_task` (`main/nvs_config.c`) e `httpd` (`main/http_server/http_server.c`, responsável pelos handlers OTA) permanecem estritamente com stacks em SRAM interna.
- **[Risco] Contenção no mutex `valid_jobs_lock` entre Core 1 (`create_jobs_task` e `ASIC_result_task`) e Core 0 (`SYSTEM_clean_jobs_queue`)** → **Mitigação:** A seção crítica realiza apenas leitura de um `uint32_t` (`version`) ou `free_bm_job` (3 `free()` rápidos) antes da escrita UART (`_send_BM1370` fica fora do lock), mantendo o tempo de retenção do mutex na ordem de poucos microssegundos.

## Migration Plan

1. Implementar as alterações de `device_config`, PSRAM/Core Pinning e correções de concorrência.
2. Validar compilação limpa (`idf.py build`) e execução dos testes unitários.
3. Como o backup integral (`bitaxe-backup/flash_16MB_601a-4_V1.0.1-rc.10.bin` e `nvs_601a-4_original.bin`) já foi realizado, em caso de necessidade de rollback basta restaurar via `esptool.py write_flash 0x0 bitaxe-backup/flash_16MB_601a-4_V1.0.1-rc.10.bin`.
