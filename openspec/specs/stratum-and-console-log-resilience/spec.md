# stratum-and-console-log-resilience Specification

## Purpose

Garante que o processamento de mensagens Stratum V1 e o subsistema de buffer de log não bloqueiem a CPU nem mantenham mutexes globais retidos durante E/S síncrona em console serial.

## Requirements

### Requirement: Rebaixamento do dump bruto JSON do Stratum V1 para nível DEBUG
O parser Stratum V1 (`STRATUM_V1_parse` em `components/stratum/stratum_api.c`) SHALL emitir o payload JSON bruto recebido (`rx: %s`) em nível `ESP_LOGD`, impedindo transmissões síncronas longas na console UART0 sob nível de log padrão `INFO`.

#### Scenario: Recepção de `mining.notify` longo com nível de log padrão INFO
- **WHEN** `STRATUM_V1_parse` processa uma notificação `mining.notify` de ~1,1 a 1,35 KB com nível de log configurado em `INFO`
- **THEN** o payload JSON bruto completo não é emitido na console `UART0` nem no ring buffer de log, evitando bloqueio >100 ms no Core 0

#### Scenario: Diagnóstico com nível DEBUG habilitado ou falha de parse JSON
- **WHEN** o nível de log para `stratum_api` é elevado para `DEBUG` ou o JSON recebido falha em `cJSON_Parse`
- **THEN** o payload bruto é registrado em nível `DEBUG` (ou `ERROR` na falha de parse) sem alterar a estrutura `StratumApiV1Message` decodificada

### Requirement: Log conciso de decodificação de coinbase em notificações Stratum
O decodificador de notificação (`decode_mining_notification` em `main/tasks/stratum_v1_task.c`) SHALL emitir os detalhes repetitivos de sinalização (`BIP-54`/`BIP-110`), `Scriptsig`, totalizador de coinbase e outputs individuais em nível `ESP_LOGD`, restringindo a emissão em `ESP_LOGI` a mudanças de altura de bloco (`block_height`).

#### Scenario: Atualização de template intra-bloco (`clean_jobs = false` ou mesma altura)
- **WHEN** chega uma nova `mining.notify` na mesma altura de bloco com alteração de taxas, `Scriptsig` ou `OP_RETURN`
- **THEN** `decode_mining_notification` atualiza `GLOBAL_STATE` normalmente e registra os detalhes de coinbase apenas em `ESP_LOGD`, sem rajada de múltiplos `ESP_LOGI` síncronos

#### Scenario: Descoberta de novo bloco na rede (`block_height` alterado)
- **WHEN** `decode_mining_notification` detecta `result->block_height != GLOBAL_STATE->block_height`
- **THEN** a nova altura de bloco é registrada uma única vez em `ESP_LOGI`

### Requirement: Liberação de s_log_mutex antes da escrita síncrona em stdout
O logger em anel (`log_buffer_vprintf` em `main/log_buffer.c`) SHALL reter `s_log_mutex` exclusivamente durante a formatação no buffer estático, a escrita em PSRAM (`ring_write`) e `websocket_log_notify()`, liberando `s_log_mutex` antes de invocar `fputs(..., stdout)` sobre uma cópia segura da linha.

#### Scenario: Task de alta prioridade no Core 1 emite log durante escrita na UART0 pelo Core 0
- **WHEN** `stratum_v1_task` ou outra task de menor prioridade executa `fputs(..., stdout)` e `ASIC_result_task` (Core 1, prioridade 15) ou `POWER_MANAGEMENT_task` (Core 0, prioridade 10) invoca `ESP_LOG*`
- **THEN** `s_log_mutex` já foi liberado antes de `fputs(..., stdout)`, permitindo que a gravação no ring buffer e WebSocket conclua sem herança de prioridade estendida ao longo da transmissão serial

#### Scenario: Falha de alocação do buffer auxiliar de stdout sob pressão de memória
- **WHEN** a alocação da cópia independente para `stdout` retorna `NULL`
- **THEN** o sistema imprime a mensagem de forma segura antes de liberar `s_log_mutex` (ou sem referenciar memória já liberada), preservando integridade contra *use-after-free* e queda silenciosa de logs
