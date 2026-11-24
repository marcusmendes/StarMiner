# Design

## Context

Veja `proposal.md` para a motivação forense completa a partir das 3 placas Bitaxe Gamma 601a em operação (`192.168.1.5`, `.21` e `.22`, firmware `e82f47a-dirty` / ESP-IDF `v5.5.3`). Os seguintes fatos e restrições do firmware orientam o desenho:

- **Stacks em PSRAM vs. ISR IRAM-safe no `I2C0`**:
  - `sdkconfig` define `CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y` e `CONFIG_I2C_ISR_IRAM_SAFE=y`.
  - `main/main.c` aloca as stacks de `POWER_MANAGEMENT_task` e `FAN_CONTROLLER_task` com `MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`. Essas tasks passam buffers locais da stack para `i2c_bitaxe_register_read/write_*()` (`main/i2c_bitaxe.c`), como `uint8_t data[2]` em `main/power/TPS546.c` e `tach_lsb`/`tach_msb` em `main/thermal/EMC2101.c`.
  - `main/display.c` configura `lvgl_cfg.task_stack_caps = MALLOC_CAP_SPIRAM`, fazendo `taskLVGL` chamar `esp_lcd_panel_io_i2c` (que não passa por `i2c_bitaxe.c`) com buffers em stack PSRAM.
  - Durante operações de Flash SPI (`nvs_commit()` via `nvs_task`, `esp_ota_write()`, `esp_partition_erase_range()`), o cache de Flash/PSRAM é temporariamente desabilitado, enquanto o ISR do `I2C0` permanece ativo em IRAM.
- **Arquitetura atual do WebSocket (`main/http_server/websocket*.c`)**:
  - `websocket_broadcast()` e `websocket_send_to_client()` chamam `httpd_ws_send_frame_async()` diretamente de tasks externas (`websocket_api_task` e `websocket_log_task`) e iteram `clients[]` sem adquirir `clients_mutex`.
  - `websocket_api.c` mantém `static int prebuffer_len = 256`, mutado tanto na thread do `httpd` (`websocket_api_on_connect()`) quanto em `websocket_api_task`.
- **Coredump e partições (`partitions.csv`, `sdkconfig`, `build/config/kconfig_menus.json`)**:
  - `partitions.csv` já define `coredump, data, coredump, , 64K`.
  - Conforme verificado no schema Kconfig do ESP-IDF v5.5.3 (`build/config/kconfig_menus.json`), `ESP_COREDUMP_ENABLE_TO_FLASH` é compatível com stacks em PSRAM quando `ESP_COREDUMP_STACK_SIZE` reserva uma stack dedicada em DRAM interna (`ESP_COREDUMP_USE_STACK_SIZE`), permitindo que o handler de panic escreva o ELF em Flash sem depender da stack em PSRAM da task que falhou.
- **Folga de memória observada em hardware**: todas as 3 placas operam com `~164 KB` de SRAM interna livre (`maxAllocHeap` `~84 KB`) e `~7,5 MB` de PSRAM livre, bem acima do piso de `85 KB` exigido pela spec `memory-and-core-pinning` e da meta de `110000` bytes verificada por `scripts/backup_and_verify_gamma_v2.sh`.

## Goals / Non-Goals

**Goals:**
- Garantir que nenhum ponteiro residente em PSRAM seja entregue ao driver `i2c_master` pelo caminho `i2c_bitaxe.c` ou pela `taskLVGL` via `esp_lcd_panel_io_i2c`.
- Eliminar completamente chamadas a `httpd_ws_send_frame_async()` fora da thread do servidor `httpd`, corridas sobre `clients[]`/`type_counts[]` e estado mutável compartilhado sem trava em `websocket_api.c`.
- Persistir coredumps em formato ELF na partição `coredump` (`64 KB`) existente e registrar automaticamente o resumo pós-morte no `log_buffer` durante o boot.
- Manter a SRAM interna livre em operação acima de `110 KB` (folga segura sobre o piso mínimo de `85 KB`).

**Non-Goals:**
- Alterar `partitions.csv` ou aumentar o tamanho da partição `coredump` além de `64 KB` (quebraria compatibilidade de layout em atualizações OTA nos dispositivos em campo).
- Mover as demais stacks de aplicação (`POWER_MANAGEMENT_task`, `FAN_CONTROLLER_task`, `stratum_*`, `create_jobs_task`, etc.) de volta para SRAM interna.
- Criar novos endpoints HTTP/campos em `openapi.yaml` ou alterações de UI no AxeOS Angular nesta change (exposição em API pode ser feita em change separada se desejado).
- Silenciar logs de boot do `wifi`/`mDNS` (os 2 eventos `STATUS_CML` residuais ocorrem apenas no boot; durante a mineração a contagem permanece zerada).

## Decisions

### Decision 1: Staging obrigatório em DRAM interna dentro de `main/i2c_bitaxe.c`
- **Escolha**:
  - Definir buffers estáticos em DRAM interna (`DRAM_ATTR static uint8_t s_i2c_tx_staging[64]` e `s_i2c_rx_staging[64]`) protegidos por `SemaphoreHandle_t s_i2c_staging_mutex` (criado em `i2c_bitaxe_init()`, com criação lazy segura para chamadas em testes).
  - Extrair um helper testável para preparação/liberação dos buffers de transferência em `i2c_transfer_with_retries()`:
    - Para `write_len <= 64` e `read_len <= 64`, adquirir `s_i2c_staging_mutex`, copiar `write_buf` para `s_i2c_tx_staging`, passar `s_i2c_tx_staging`/`s_i2c_rx_staging` ao ciclo de tentativas `i2c_master_transmit*()` + `i2c_master_bus_reset()`, copiar `s_i2c_rx_staging` de volta ao `read_buf` do chamador somente após retorno de `ESP_OK` e liberar o mutex.
    - Para payloads `> 64 B`, alocar buffers temporários via `heap_caps_malloc(..., MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)` (retornando `ESP_ERR_NO_MEM` caso falhe) e liberá-los antes de retornar.
- **Alternativas consideradas**:
  - *Mover `POWER_MANAGEMENT_task` (`8 KB`), `FAN_CONTROLLER_task` (`8 KB`) e futuras tasks para SRAM interna*: consumiria `16+ KB` extras de SRAM interna e continuaria vulnerável caso qualquer nova task em PSRAM acessasse o barramento `I2C0`. O staging no ponto único (`i2c_bitaxe.c`) protege todos os chamadores atuais e futuros gastando apenas `128 B` de DRAM estática.

### Decision 2: Stack da `taskLVGL` em SRAM interna (`main/display.c`)
- **Escolha**:
  - Em `display_init()`, alterar `lvgl_cfg.task_stack_caps = MALLOC_CAP_SPIRAM;` para `lvgl_cfg.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;` mantendo `lvgl_cfg.task_affinity = 0`.
- **Rationale**:
  - O driver `esp_lcd_panel_io_i2c` / `esp_lcd_sh1107` / `esp_lcd_panel_ssd1306` é executado no contexto de `taskLVGL` e passa literais compostos da stack (`(uint8_t[]){ ... }`) diretamente para o barramento I2C sem passar por `i2c_bitaxe.c`. Como `taskLVGL` usa apenas `7168 B` de stack (`ESP_LVGL_PORT_INIT_CONFIG()`), alocá-la em SRAM interna fecha esse vetor sem comprometer a meta de `freeHeapInternal >= 110 KB`.
- **Alternativas consideradas**:
  - *Substituir `esp_lcd_panel_io_i2c` por wrapper customizado*: acoplaria drivers gerenciados do ESP-IDF (`managed_components/espressif__esp_lcd_sh1107`) a código duplicado sem ganho relevante frente ao custo de `7 KB` em SRAM.

### Decision 3: Serialização WebSocket via `httpd_queue_work()` e *backpressure* por tipo
- **Escolha**:
  - Definir uma estrutura de trabalho `ws_work_item_t` contendo cópia própria do payload (alocada preferencialmente em PSRAM com fallback padrão), comprimento, tipo de frame (`httpd_ws_type_t`), `WebSocketClientType` e `target_fd` (`-1` para broadcast; `>= 0` para envio direcionado).
  - Em `websocket_broadcast()` e `websocket_send_to_client()`:
    1. Verificar sob `clients_mutex` se há clientes elegíveis (para broadcast, ao menos 1 cliente ativo do `type`; para envio direcionado, se `target_fd` está ativo) e se o contador de pendentes do respectivo `type` está abaixo do teto (`WS_MAX_PENDING_LOGS = 8`, `WS_MAX_PENDING_API = 4`). Caso contrário, descartar sem alocar.
    2. Incrementar `s_pending_work[type]` sob `clients_mutex`, alocar `ws_work_item_t` + cópia do payload e chamar `httpd_queue_work(server_handle, ws_send_work_fn, item)`. Se `httpd_queue_work()` falhar, decrementar `s_pending_work[type]` e liberar memória imediatamente.
  - No callback `ws_send_work_fn(void *arg)` (executado exclusivamente na thread do `httpd`):
    1. Adquirir `clients_mutex` apenas para copiar o subconjunto de `fd`s válidos para um array local na stack (`int fds[MAX_WEBSOCKET_CLIENTS]`) e decrementar `s_pending_work[type]` (ou decrementar no `finally` do callback). Liberar `clients_mutex` antes de qualquer E/S de rede.
    2. Para cada `fd` copiado, construir `httpd_ws_frame_t` com `final = true` apontando para a cópia própria do payload e chamar `httpd_ws_send_frame_async(server_handle, fd, &pkt)`. Se falhar, apenas registrar em `ESP_LOGW` (a limpeza definitiva do socket continua sob responsabilidade de `websocket_close_fn`).
    3. Liberar `item->payload` e `item`.
  - Em `main/http_server/websocket_api.c`, remover a corrida sobre `static int prebuffer_len`: `websocket_api_on_connect(int fd)` passa a usar tamanho de pré-alocação local constante (ex.: `4096 B`), enquanto `websocket_api_task()` mantém seu `prebuffer_len` exclusivamente local à sua única thread.
- **Alternativas consideradas**:
  - *Proteger `httpd_ws_send_frame_async()` apenas com um mutex externo compartilhado*: não garante segurança interna no `esp_http_server` porque a thread `httpd` também manipula estruturas internas da sessão/socket fora dos hooks da aplicação. `httpd_queue_work()` é o mecanismo oficial documentado pelo ESP-IDF v5.5 para envios WebSocket assíncronos fora de handlers HTTP.

### Decision 4: Coredump ELF em Flash (64 KB) + resumo automático no boot
- **Escolha**:
  - Adicionar em `sdkconfig.defaults` (e sincronizar via `idf.py reconfigure` para `sdkconfig`):
    - `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`
    - `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`
    - `CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y`
    - `CONFIG_ESP_COREDUMP_CHECK_BOOT=y`
    - `CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=32`
    - `CONFIG_ESP_COREDUMP_STACK_SIZE=2048`
  - Manter `CONFIG_ESP_COREDUMP_CAPTURE_DRAM` desabilitado (`n`, padrão) para que o ELF contenha cabeçalhos, registradores, TCBs e stacks ativas sem ultrapassar os `64 KB` reservados na partição `coredump` de `partitions.csv`.
  - Adicionar `"espcoredump"` em `PRIV_REQUIRES` de `main/CMakeLists.txt`.
  - Em `main/main.c`, criar helper `log_stored_coredump_summary()` chamado logo após `log_buffer_init()` (protegido por `#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`):
    - Chama `esp_core_dump_image_check()`. Se `ESP_OK`, aloca `esp_core_dump_summary_t` no heap, chama `esp_core_dump_get_summary(summary)` e registra `exc_task`, `exc_pc`, `ex_info.exc_cause`/`exc_vaddr` (quando aplicável à arquitetura Xtensa) e os endereços de `exc_bt_info.bt[]` em `ESP_LOGE` quando `esp_reset_reason() == ESP_RST_PANIC` (ou `ESP_LOGI` informativo quando houver dump antigo em boot não-panic), liberando o struct em seguida e preservando a partição para leitura detalhada opcional com `espcoredump.py`.
- **Alternativas consideradas**:
  - *Coredump para UART (`CONFIG_ESP_COREDUMP_ENABLE_TO_UART=y`)*: inútil quando os mineradores operam sem cabo serial conectado em produção; o `log_buffer` em PSRAM só captura logs emitidos após `log_buffer_init()`, nunca a saída do panic handler na ROM UART.
  - *Aumentar a partição `coredump` para `128K` em `partitions.csv`*: deslocaria partições subsequentes ou exigiria encolher OTA/SPIFFS, quebrando atualização OTA direta nas placas atuais.

## Risks / Trade-offs

- **Consumo de SRAM interna (~9,3 KB)** -> `taskLVGL` (`7168 B`), `CONFIG_ESP_COREDUMP_STACK_SIZE` (`2048 B`), staging I2C (`128 B`) e eventual alocação de logs do componente `espcoredump` reduzem a SRAM interna livre de `~164 KB` para `~149–155 KB`, permanecendo bem acima do piso de `85 KB` e da meta de `110 KB`. Validar o valor exato de `freeHeapInternal` após build e boot real.
- **Capacidade da partição `coredump` (`64 KB`)** -> Um ELF com muitas tasks simultâneas pode exceder `64 KB` se todas estiverem com uso profundo de stack ou se `CAPTURE_DRAM` fosse ligado. Mitigado mantendo `CONFIG_ESP_COREDUMP_CAPTURE_DRAM=n` e `CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=32`, além de testar a gravação e leitura de resumo em bancada.
- **Saturação de `httpd_queue_work` sob burst de logs** -> Como `httpd_queue_work` usa a fila de controle interna do `httpd`, limites estritos de pendência (`WS_MAX_PENDING_LOGS = 8`, `WS_MAX_PENDING_API = 4`) descartam excesso de quadros antes do enfileiramento sem vazar memória.
- **Testabilidade em QEMU (`test/` e `test-ci/`)** -> `test-ci` compila `log_buffer.c` com stub de `websocket_log_notify()`. Novos testes unitários para staging I2C e lógica de registro/backpressure WebSocket devem evitar dependência de hardware real ou isolar stubs onde necessário para manter `./tools/run_qemu_tests.sh` verde.

## Migration Plan

1. Compilar o firmware completo (`idf.py build`) e executar a suíte QEMU (`./tools/run_qemu_tests.sh`).
2. Antes de gravar nas placas em produção (`192.168.1.5`, `.21`, `.22`), executar backup via `./scripts/backup_and_verify_gamma_v2.sh backup-http <ip>` (e `backup-flash` via serial na unidade piloto).
3. Gravar inicialmente em 1 placa piloto (ex.: `192.168.1.22`, onde ocorreu o último panic), validar `verify-post-flash` (`freeHeapInternal >= 110000`) e submeter a estresse de UI WebSocket + escrita NVS antes de atualizar as outras 2 unidades.
4. Rollback imediato: como `partitions.csv` e NVS não mudam, qualquer unidade pode reverter via OTA para o binário anterior sem perda de configuração.
