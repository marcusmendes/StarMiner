# memory-and-core-pinning Specification

## Purpose

Define a política de alocação de stacks de tasks e buffers de heap em PSRAM (com preservação das tasks que gravam na Flash SPI em SRAM interna) e o isolamento determinístico de execução entre os núcleos Core 0 e Core 1 do ESP32-S3.

## Requirements

### Requirement: Migração de stacks de tasks elegíveis para PSRAM com fallback seguro
O sistema SHALL alocar as stacks das tasks de aplicação que não realizam escrita direta em Flash SPI (`POWER_MANAGEMENT_task`, `FAN_CONTROLLER_task`, `create_jobs_task`, `ASIC_result_task`, `hashrate_monitor_task`, `statistics_task`, `protocol_coordinator_task`, `stratum_v1_task`, `stratum_v2_task`, `websocket_log_task`, `websocket_api_task` e tasks `BAP`) na PSRAM quando inicializada, recaindo para memória padrão caso a PSRAM não esteja disponível.

#### Scenario: Inicialização de tasks com PSRAM ativa
- **WHEN** `esp_psram_is_initialized()` retorna verdadeiro durante a criação das tasks elegíveis
- **THEN** as stacks dessas tasks são alocadas com capacidades `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`, preservando a SRAM interna para Wi-Fi, DMA, ISRs e operações de Flash

#### Scenario: Inicialização de tasks sem PSRAM disponível
- **WHEN** `esp_psram_is_initialized()` retorna falso
- **THEN** as tasks elegíveis são criadas utilizando memória padrão (`MALLOC_CAP_DEFAULT`) sem abortar a inicialização

### Requirement: Preservação obrigatória de tasks de escrita em Flash na SRAM interna
O sistema MUST manter as stacks de `nvs_task` e da task do servidor HTTP (`httpd`) alocadas exclusivamente na SRAM interna.

#### Scenario: Atualização OTA ou gravação na NVS
- **WHEN** o servidor `httpd` executa `esp_ota_write` / `esp_partition_erase_range` ou a `nvs_task` grava dados na Flash SPI
- **THEN** a stack da task em execução reside em SRAM interna, evitando falhas de cache (*Cache disabled but cached memory region accessed*) durante operações na Flash SPI

### Requirement: Alocação de buffers de rede, UART e mineração na PSRAM
O sistema SHALL alocar em PSRAM (quando `esp_psram_is_initialized()` for verdadeiro) o contexto `rest_server_context_t` (`10,5 KB`), o `scratch_buf` do WebSocket (`4 KB`), o buffer RX do BAP (`1 KB`), as estruturas/strings `bm_job` e o `json_rpc_buffer` / `mining_notify` do Stratum V1.

#### Scenario: Alocação de buffers de trabalho e HTTP em operação normal
- **WHEN** o servidor HTTP, o cliente Stratum V1, o gerador de jobs ou o receptor UART BAP alocam seus buffers dinâmicos com PSRAM inicializada
- **THEN** as alocações utilizam `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT` (com fallback para `MALLOC_CAP_DEFAULT`), mantendo a SRAM interna livre acima de `85 KB`

### Requirement: Isolamento de núcleos (Core Pinning) entre Mineração e Sistema
O sistema SHALL fixar exclusivamente no **Core 1** as tasks de tempo real de mineração (`ASIC_result_task` com prioridade 15, `create_jobs_task` com prioridade 10 e `hashrate_monitor_task` com prioridade 5) e fixar no **Core 0** o servidor `httpd` e as demais tasks de sistema, rede, Stratum, energia, ventilação, estatísticas, NVS e BAP.

#### Scenario: Recepção de nonces do ASIC sob tráfego intenso de Wi-Fi e HTTP
- **WHEN** o Core 0 processa interrupções de Wi-Fi, requisições HTTP/WebSocket ou mensagens Stratum
- **THEN** `ASIC_result_task` (prioridade 15) e `create_jobs_task` (prioridade 10) executam isoladas no Core 1 sem preempção pelas tasks de rede ou sensores do Core 0
