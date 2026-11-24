# Tasks

## 1. Staging I2C em SRAM Interna e Stack Interna da `taskLVGL`

- [x] 1.1 Em `main/i2c_bitaxe.c`, implementar *staging* obrigatório em DRAM interna (`DRAM_ATTR static uint8_t s_i2c_tx_staging[64]` e `s_i2c_rx_staging[64]` protegidos por `s_i2c_staging_mutex`, com alocação temporária `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` para payloads `> 64 B` e retorno `ESP_ERR_NO_MEM` em falha), extraindo helper testável usado por `i2c_transfer_with_retries()` que preserva logs de falha, `i2c_master_bus_reset()` e cópia segura para `read_buf` apenas após `ESP_OK`; validar compilação limpa sem warnings no alvo `esp32s3`.
- [x] 1.2 Em `main/display.c`, alterar `lvgl_cfg.task_stack_caps` de `MALLOC_CAP_SPIRAM` para `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` em `display_init()` mantendo `lvgl_cfg.task_affinity = 0`, verificando por inspeção de compilação e execução em hardware que `taskLVGL` permanece no Core 0 sem buffers em PSRAM no caminho `esp_lcd_panel_io_i2c`.
- [x] 1.3 Adicionar testes unitários em `test/main/` (registrados também em `test/main/CMakeLists.txt` e `test-ci/main/CMakeLists.txt`) cobrindo preparação de buffers de *staging* TX/RX `<= 64 B` e `> 64 B`, garantia de `esp_ptr_internal()` para ponteiros entregues ao driver e liberação correta de recursos; verificar aprovação via `./tools/run_qemu_tests.sh`.

## 2. Serialização e Thread-Safety do Subsistema WebSocket

- [x] 2.1 Em `main/http_server/websocket.h` e `main/http_server/websocket.c`, refatorar `websocket_broadcast()` e `websocket_send_to_client()` para despachar cópias próprias do payload à thread `httpd` via `httpd_queue_work()`, protegendo toda leitura/escrita de `clients[]` e `type_counts[]` com `clients_mutex`, revalidando `fd`s registrados dentro do callback `httpd` antes de chamar `httpd_ws_send_frame_async()`, liberando `clients_mutex` antes da E/S de rede e impondo limites de *backpressure* por canal (`WS_MAX_PENDING_LOGS = 8` e `WS_MAX_PENDING_API = 4`); verificar ausência de chamadas a `httpd_ws_send_frame_async()` fora do worker `httpd` e ausência de vazamento em falhas de fila.
- [x] 2.2 Em `main/http_server/websocket_api.c` e `main/http_server/websocket_log.c`, remover a mutação compartilhada sem trava de `static int prebuffer_len` (usando pré-alocação local constante em `websocket_api_on_connect()` e mantendo `prebuffer_len` estritamente local à `websocket_api_task`) e assegurar construção segura de `httpd_ws_frame_t` (`final = true`); validar compilação limpa e funcionamento contínuo dos streams `/api/ws` e `/api/ws/live`.
- [x] 2.3 Adicionar testes unitários em `test/main/` (integrados em `test/main/CMakeLists.txt` e `test-ci/main/CMakeLists.txt`) validando registro/remoção concorrente de clientes sob `clients_mutex`, revalidação de `fd` desconectado antes do envio, respeito aos tetos de *backpressure* (`8` para logs e `4` para API) e liberação do payload em falha de enfileiramento; verificar aprovação via `./tools/run_qemu_tests.sh`.

## 3. Coredump ELF em Flash e Diagnóstico Automático no Boot

- [x] 3.1 Habilitar `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`, `CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y`, `CONFIG_ESP_COREDUMP_CHECK_BOOT=y`, `CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=32` e `CONFIG_ESP_COREDUMP_STACK_SIZE=2048` em `sdkconfig.defaults`, executar `idf.py reconfigure` para sincronizar `sdkconfig` mantendo `partitions.csv` inalterado (`coredump` de `64K`), e confirmar no `sdkconfig` gerado que `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y` permanece ativo com `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y`.
- [x] 3.2 Adicionar `"espcoredump"` em `PRIV_REQUIRES` de `main/CMakeLists.txt` e implementar `log_stored_coredump_summary()` em `main/main.c` logo após `log_buffer_init()` (protegido por `#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`), consultando `esp_core_dump_image_check()` e `esp_core_dump_get_summary()` (alocado no heap) para registrar `exc_task`, `exc_pc`, causa e endereços de *backtrace* no `log_buffer` sem apagar a partição; verificar compilação com `idf.py build` sem warnings.

## 4. Verificação Integrada de Build, Testes e Hardware

- [x] 4.1 Executar build completo na raiz (`idf.py build`) e suíte completa de testes unitários em QEMU (`./tools/run_qemu_tests.sh`, além de `test/`), confirmando 0 erros de compilação, `0` falhas nos testes e cobertura completa das regressões adicionadas.
- [x] 4.2 Realizar backup prévio da unidade piloto (`192.168.1.22`) com `./scripts/backup_and_verify_gamma_v2.sh backup-http 192.168.1.22` (e `backup-flash` se conectada via serial), gravar o novo firmware e validar com `./scripts/backup_and_verify_gamma_v2.sh verify-post-flash 192.168.1.22` (`freeHeapInternal >= 110000`, `0` erros `power_fault`), executar teste combinado de estresse WebSocket (`/api/ws`, `/api/ws/live`, Swarm) + gravações em NVS (`PATCH /api/system`) confirmando `0` ocorrências de `ESP_RST_PANIC` e leitura limpa de `esp_core_dump_image_check()`, e em seguida replicar o rollout para `192.168.1.5` e `192.168.1.21`.

## Workflow follow-up

- Após implementação e validação em hardware, arquivar a mudança com `/openspec-archive-change` e sincronizar os specs principais (`i2c-and-tps546-resilience`, `memory-and-core-pinning`, `websocket-thread-safety` e `flash-coredump-diagnostics`).
