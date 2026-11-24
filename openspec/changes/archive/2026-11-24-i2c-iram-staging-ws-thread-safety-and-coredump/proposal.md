# Proposal

## Why

A análise forense das 3 placas Bitaxe Gamma 601a em produção (`192.168.1.5`, `192.168.1.21` e `192.168.1.22`, firmware `e82f47a-dirty` / ESP-IDF `v5.5.3`) mostrou que, embora o bloqueio de console Stratum e os erros `TPS546 STATUS_CML=0x02` durante mineração tenham sido eliminados pela mudança anterior, **as 3 unidades registraram reinícios silenciosos por `ESP_RST_PANIC`** (`"Software reset due to exception/panic"`). Na unidade `.22`, o panic ocorreu em operação normal (`t = 8046,7 s`) sem nenhum log prévio de erro ou aviso, exatamente `2 s` antes de a interface web/Swarm abrir conexões HTTP e `/api/ws/live`, e sem backtrace salvo em Flash porque `CONFIG_ESP_COREDUMP_ENABLE_TO_NONE=y` está ativo mesmo já existindo a partição `coredump` de `64 KB` em `partitions.csv`.

A auditoria do código atual identificou **dois mecanismos ativos de panic** e **uma lacuna crítica de observabilidade**:

1. **Buffers em stack PSRAM passados ao barramento `I2C0` com `CONFIG_I2C_ISR_IRAM_SAFE=y`**: a partir do commit `1042171`, o ISR do `I2C0` permanece ativo em IRAM quando operações de Flash SPI (`nvs_commit()`, `esp_ota_write()`, `esp_partition_erase_range()`) desabilitam o cache de Flash/PSRAM. Contudo, `POWER_MANAGEMENT_task` e `FAN_CONTROLLER_task` têm stacks em PSRAM (`main/main.c`) e passam buffers locais da stack diretamente para `i2c_bitaxe_register_*()` (`main/i2c_bitaxe.c`), enquanto `taskLVGL` também usa `MALLOC_CAP_SPIRAM` (`main/display.c`) e passa literais/buffers de stack para `esp_lcd_panel_io_i2c`. Qualquer sobreposição com escrita em Flash provoca panic imediato (`Cache disabled but cached memory region accessed`).
2. **Concorrência não *thread-safe* no WebSocket (`httpd_ws_send_frame_async`)**: `main/http_server/websocket.c` e `main/http_server/websocket_api.c` invocam `httpd_ws_send_frame_async()` diretamente a partir de tasks externas (`websocket_api_task` e `websocket_log_task`) sem passar pelo contexto da thread `httpd` (`httpd_queue_work()`), iteram `clients[]`/`type_counts[]` sem proteção de `clients_mutex` e compartilham `static int prebuffer_len` sem sincronização entre `httpd` (`websocket_api_on_connect`) e `websocket_api_task`.
3. **Ausência de captura de coredump em Flash**: embora `partitions.csv` já reserve a partição `coredump` (`64K`), o firmware descarta todos os panics sem gravar o dump nem registrar o resumo pós-morte no `log_buffer` após o boot.

Resolver esses pontos blinda a estabilidade e o *uptime* do firmware antes da introdução de camadas adaptativas de controle e otimização.

## What Changes

- **Staging obrigatório de transferências `I2C0` em SRAM interna (`main/i2c_bitaxe.c`)**:
  - Copiar payloads de escrita e receber leituras em buffers de *staging* residentes em SRAM interna (`DRAM_ATTR` estático de `64 B` protegido por mutex dedicado, com alocação temporária `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` para transferências `> 64 B`, retornando `ESP_ERR_NO_MEM` em falha), garantindo que `i2c_master_transmit*()` nunca receba ponteiros de PSRAM.
- **Alocação da stack da `taskLVGL` em SRAM interna (`main/display.c`)**:
  - Alterar `lvgl_cfg.task_stack_caps` de `MALLOC_CAP_SPIRAM` para `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` em `display_init()`, mantendo `lvgl_cfg.task_affinity = 0` e os buffers de desenho monocromáticos em DRAM interna, preservando `>= 85 KB` de SRAM interna livre.
- **Serialização *thread-safe* e backpressure no subsistema WebSocket (`main/http_server/websocket.h`, `websocket.c`, `websocket_api.c`, `websocket_log.c`)**:
  - Enfileirar todos os envios WebSocket (`websocket_broadcast` e `websocket_send_to_client`) para execução exclusiva na thread do servidor HTTP via `httpd_queue_work()`, usando cópia própria do payload liberada apenas após o envio ou falha de enfileiramento.
  - Proteger todas as leituras/escritas de `clients[]` e `type_counts[]` com `clients_mutex`, revalidar `fd` ativo dentro do worker antes de chamar `httpd_ws_send_frame_async()` e aplicar limites de *backpressure* por tipo (`WS_TYPE_LOGS` / `WS_TYPE_API`) para evitar esgotamento da fila do `httpd`.
  - Eliminar a mutação concorrente de `static int prebuffer_len` sem sincronização em `websocket_api.c`.
- **Habilitação de coredump ELF em Flash e registro automático no boot (`sdkconfig.defaults`, `sdkconfig`, `main/CMakeLists.txt`, `main/main.c`)**:
  - Habilitar `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`, `CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y`, `CONFIG_ESP_COREDUMP_CHECK_BOOT=y`, `CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=32` e `CONFIG_ESP_COREDUMP_STACK_SIZE=2048` reutilizando integralmente a partição `coredump` (`64K`) existente em `partitions.csv` (sem alterar o layout de partições).
  - Incluir `espcoredump` em `main/CMakeLists.txt` e registrar no boot após `log_buffer_init()` (`main/main.c`) o resumo do coredump (`esp_core_dump_image_check()` + `esp_core_dump_get_summary()`) com `exc_task`, `exc_pc`, causa e endereços de *backtrace* no `log_buffer`, sem apagar automaticamente a partição.

## Capabilities

### New Capabilities
- `websocket-thread-safety`: Garante que todo envio de frames WebSocket seja serializado na thread `httpd` via `httpd_queue_work()`, com acesso sincronizado à tabela de clientes, revalidação de `fd` antes da transmissão, controle de *backpressure* e ausência de estado mutável compartilhado sem trava em `websocket_api`.
- `flash-coredump-diagnostics`: Habilita a persistência de coredumps em formato ELF na partição `coredump` de `64 KB` em Flash com stack interna dedicada e registra automaticamente no boot o resumo pós-morte (`exc_task`, `exc_pc`, causa e *backtrace*) no `log_buffer`.

### Modified Capabilities
- `i2c-and-tps546-resilience`: Adiciona o requisito de *staging* obrigatório de buffers TX/RX do barramento `I2C0` em SRAM interna (`DRAM_ATTR` / `MALLOC_CAP_INTERNAL`) para prevenir acessos à PSRAM pelo ISR IRAM-safe durante operações em Flash.
- `memory-and-core-pinning`: Atualiza a política de alocação da stack de `taskLVGL` em `display_init()` para `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` (mantendo `task_affinity = 0`), eliminando buffers de stack em PSRAM passados a `esp_lcd_panel_io_i2c` enquanto preserva o piso de `>= 85 KB` de SRAM interna livre.

## Impact

- **Código e Configuração Afetados**:
  - `main/i2c_bitaxe.c` (buffers `DRAM_ATTR` de staging, mutex dedicado e fallback `MALLOC_CAP_INTERNAL`)
  - `main/display.c` (`lvgl_cfg.task_stack_caps` para SRAM interna)
  - `main/http_server/websocket.h`, `main/http_server/websocket.c`, `main/http_server/websocket_api.c`, `main/http_server/websocket_log.c` (serialização via `httpd_queue_work()`, locks e limites de pendência)
  - `sdkconfig.defaults`, `sdkconfig`, `main/CMakeLists.txt`, `main/main.c` (habilitação de coredump ELF em Flash e log de resumo no boot)
  - `test/` e `test-ci/` (testes unitários de staging I2C em SRAM interna e ciclo de vida/backpressure WebSocket)
- **Compatibilidade e Contratos Preservados**:
  - **Sem** alterações em `partitions.csv` (compatibilidade total com dispositivos já gravados via OTA).
  - **Sem** alterações em `main/http_server/openapi.yaml`, endpoints HTTP/WebSocket existentes, chaves NVS ou frontend Angular (`AxeOS`).
