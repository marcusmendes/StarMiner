# Design

## Context

Veja `proposal.md` para a motivação e a correlação estatística observada nos logs de produção (`7/7` falhas `TPS546 STATUS_CML=0x02` ocorrendo exclusivamente `+122` a `+168 ms` após `mining.notify`). Os seguintes invariantes e restrições do firmware moldam a implementação:

- **Console UART0 síncrona (`sdkconfig`)**: `CONFIG_ESP_CONSOLE_UART_DEFAULT=y` a `115.200 baud` (`~11,52 bytes/ms`), sem driver por interrupção na `UART0`, e com `CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y`. Cada byte impresso via `fputs(..., stdout)` consome tempo de CPU bloqueando a task chamadora e segurando o *flock* interno de `stdout`.
- **Stacks reduzidas em tasks de controle e rede**: `sys_evt` (`CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=2304`), `protocol_coordinator_task` (`3072 B`) e tasks BAP (`bap_mode_mgmt` `2048 B`) chamam `ESP_LOG*` diretamente; buffers grandes na stack dentro de `log_buffer_vprintf()` criam risco de *stack overflow*.
- **Invariante de estado em `decode_mining_notification()`**: embora os logs de `Scriptsig` e `Coinbase outputs` sejam emitidos a cada template, `GLOBAL_STATE->scriptsig`, `GLOBAL_STATE->block_signals*`, `GLOBAL_STATE->coinbase_outputs` e totalizadores em `main/tasks/stratum_v1_task.c:105-165` alimentam o display OLED (`main/screen.c`) e a API HTTP/WebSocket. Apenas o nível de log deve mudar; a atualização de `GLOBAL_STATE` deve permanecer integral.
- **Alocação PSRAM e hook de OOM (`main/main.c:42-48`)**: todas as tasks de sistema/mineração operam com stacks em PSRAM (`MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT`), e `heap_alloc_failed_hook()` chama `abort()` em falhas explícitas de `MALLOC_CAP_SPIRAM`.

## Goals / Non-Goals

**Goals:**
- Reduzir em **>90%** o tempo gasto em E/S síncrona na `UART0` e em chamadas `esp_cache_msync()` durante o processamento rotineiro de `mining.notify` (~`160 ms` → `<10 ms`).
- Eliminar a retenção de `s_log_mutex` durante `fputs(..., stdout)`, evitando bloqueios cruzados entre `Core 0` (`POWER_MANAGEMENT_task`, `websocket_log_task`) e `Core 1` (`ASIC_result_task`, `create_jobs_task`).
- Blindar o ISR do barramento `I2C0` contra interferência de cache PSRAM (`CONFIG_I2C_ISR_IRAM_SAFE=y`) e impedir que `taskLVGL` execute no Core 1.
- Expor visibilidade explícita (`ESP_LOGW`) sempre que `i2c_transfer_with_retries()` falhar e acionar `i2c_master_bus_reset()`.

**Non-Goals:**
- Reduzir `I2C_BUS_SPEED_HZ` (`main/i2c_bitaxe.h:6`) de `400 kHz` para `100 kHz` neste momento — será reavaliado apenas como contingência se testes de longa duração ainda registrarem `STATUS_CML=0x02` após a eliminação do bloqueio Stratum/UART0.
- Instalar driver UART assíncrono (`uart_driver_install`) na `UART0` ou implementar fila assíncrona separada para `stdout`, evitando consumo adicional de SRAM interna e mudanças em console ROM/JTAG.
- Adicionar mutex de software compartilhado entre `i2c_bitaxe.c` e `esp_lcd_panel_io_i2c` (o driver `i2c_master` v5 do ESP-IDF já serializa transações no barramento; o risco crítico em `i2c_bitaxe.c` vem de retries silenciosos + contenção de ISR/cache).

## Decisions

### Decision 1: Rebaixamento de verbosidade em `STRATUM_V1_parse()` e `decode_mining_notification()`
- **Escolha:**
  1. Em `STRATUM_V1_parse()` (`components/stratum/stratum_api.c:585`), alterar `ESP_LOGI(TAG, "rx: %s", stratum_json)` para `ESP_LOGD(TAG, "rx: %s", stratum_json)`. O `parse_mining_notify()` já possui `ESP_LOGD(TAG, "Parsed mining.notify: job_id=%s, clean_jobs=%d", ...)` na linha `363`, e mensagens de controle raras (`set_difficulty`, `set_version_mask`, erros de parse) permanecem em `ESP_LOGI`/`ESP_LOGE`.
  2. Em `decode_mining_notification()` (`main/tasks/stratum_v1_task.c:109-165`), preservar intactas todas as atualizações de `GLOBAL_STATE` (incluindo `scriptsig`, `block_signals`, `coinbase_outputs`, `coinbase_value_*`), mantendo apenas `ESP_LOGI(TAG, "Block height %d", result->block_height)` em nível `INFO` e rebaixando `BIP-54`, `BIP-110`, `Scriptsig`, `Coinbase outputs` e `Output %d` para `ESP_LOGD`.
- **Alternativas consideradas:**
  - *Logar um resumo resumido de `mining.notify` em `ESP_LOGI` dentro de `STRATUM_V1_parse()`*: rejeitado porque `create_jobs_task` (`main/tasks/create_jobs_task.c:125`) já registra `New Work Dequeued <job_id>` em `ESP_LOGI` imediatamente ao desenfileirar o job; duplicar outra linha por notificação manteria overhead desnecessário na `UART0`.
  - *Detectar mudança de endereços sem valores/OP_RETURN para emitir outputs em `ESP_LOGI` apenas uma vez*: desnecessário para operação normal em `INFO`, pois `Output 0..2` e `Scriptsig` seguem acessíveis habilitando `ESP_LOG_DEBUG` ou via AxeOS (`/api/system/info`), sem adicionar lógica de comparação parcial sujeita a edge cases em pools distintos.

### Decision 2: Liberação de `s_log_mutex` antes de `fputs(..., stdout)` em `log_buffer_vprintf()`
- **Escolha:**
  1. Quando `output == s_vprintf_buffer`, após `ring_write(output, needed)` e `websocket_log_notify()`, alocar um buffer independente `stdout_buf` de tamanho `needed + 1` via `malloc(needed + 1)` antes de liberar `s_log_mutex` e copiar `memcpy(stdout_buf, output, needed + 1)`.
  2. Quando `output != s_vprintf_buffer` (caminho de linhas `>= 2048 B`), `output` já pertence exclusivamente à chamada atual — atribuir `stdout_buf = output` sem segunda cópia.
  3. Chamar `xSemaphoreGiveRecursive(s_log_mutex)` **antes** de `fputs(stdout_buf, stdout)`, seguido de `free(stdout_buf)` (quando aplicável).
  4. **Fallback seguro**: se a alocação de `stdout_buf` retornar `NULL`, executar `fputs(output, stdout)` ainda com `s_log_mutex` retido e só então liberar o mutex, garantindo que nenhuma linha se perca nem leia `s_vprintf_buffer` concorrentemente.
- **Racional e interação com newlib `stdout`:** Liberar `s_log_mutex` antes de `fputs()` desbloqueia imediatamente `log_buffer_read_absolute()` (leitor HTTP/WebSocket) e evita que tasks de alta prioridade fiquem presas em `s_log_mutex` aguardando `esp_cache_msync` ou formatação de outro thread. Como o `FILE *stdout` da newlib mantém seu próprio lock interno durante `fputs()`, essa mudança funciona em conjunto com a **Decisão 1** (que elimina linhas de 1,3 KB em `INFO`) para manter o tempo de retenção da serial em poucos milissegundos.
- **Alternativas consideradas:**
  - *Buffer de cópia na stack (ex.: `char local_buf[256]`) *: rejeitado porque `log_buffer_vprintf()` executa na stack da task chamadora, onde `bap_mode_mgmt` (`2048 B`) e `sys_evt` (`2304 B`) possuem margem apertada.
  - *Usar `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)` diretamente*: rejeitado como alocador primário por acionar `abort()` em `heap_alloc_failed_hook()` (`main/main.c:42-48`) sob eventual esgotamento de PSRAM; `malloc()` utiliza alocador padrão com fallback controlado.

### Decision 3: `CONFIG_I2C_ISR_IRAM_SAFE=y` em `sdkconfig.defaults` e `sdkconfig`
- **Escolha:** Adicionar `CONFIG_I2C_ISR_IRAM_SAFE=y` em `sdkconfig.defaults` (e atualizar `sdkconfig` / `idf.py reconfigure`) mantendo `CONFIG_I2C_MASTER_ISR_HANDLER_IN_IRAM=y`.
- **Alternativas consideradas:**
  - *Manter `# CONFIG_I2C_ISR_IRAM_SAFE is not set` esperando que a redução de logs zere as colisões*: rejeitado porque ainda existem gravações periódicas de NVS/cache (ex.: `bestdiff` em `nvs_task`) e eventos WebSocket/PSRAM; garantir o ISR I2C em IRAM elimina vulnerabilidade residual a bloqueios de cache.

### Decision 4: Helper compartilhado e registro `ESP_LOGW` em `i2c_transfer_with_retries()`
- **Escolha:** Extrair um helper estático `find_device_map_entry(dev_handle)` em `main/i2c_bitaxe.c` reutilizado tanto no bloco de erro fatal (linhas `58-64`) quanto antes de invocar `i2c_master_bus_reset()` (`linhas 49-55`), emitindo:
  `ESP_LOGW(TAG, "I2C transfer failed for [%s] (0x%02x) on attempt %d/%d (%s) - resetting bus", ...)` (com rótulo `"unknown"` caso não cadastrado).
- **Racional:** Cada falha transitória hoje dispara `i2c_master_bus_reset()` silenciosamente, mascarando se o erro original veio de `TPS546` ou `EMC2101` antes do `STATUS_CML=0x02`.
- **Alternativas consideradas:**
  - *Apenas contador sem log*: insuficiente para correlacionar timestamps de retry I2C com `mining.notify` e mensagens `TPS546` no monitor serial.

### Decision 5: Afinidade explícita de `taskLVGL` ao Core 0
- **Escolha:** Em `display_init()` (`main/display.c:94-96`), configurar `lvgl_cfg.task_affinity = 0;` imediatamente junto a `lvgl_cfg.task_stack_caps = MALLOC_CAP_SPIRAM;`.
- **Alternativas consideradas:**
  - *Deixar `task_affinity = -1`*: permite que o FreeRTOS agende `taskLVGL` (prioridade 4) no Core 1 sempre que o Core 0 sofrer picos de rede/Stratum, competindo com `ASIC_result_task`/`create_jobs_task` e paralelizando transações `esp_lcd_panel_io_i2c` entre núcleos.

## Risks / Trade-offs

- **Consumo adicional de IRAM interna com `CONFIG_I2C_ISR_IRAM_SAFE=y`** → Verificar no resumo de memória do `idf.py build` que a IRAM e a SRAM interna livre permanecem confortavelmente acima do piso operacional (`>= 110 KB` de SRAM interna livre documentado no projeto).
- **Perda do JSON bruto de `mining.notify` e outputs de coinbase nos logs padrão `INFO`** → Mitigado por preservação integral em `ESP_LOGD` (ativável dinamicamente via nível de log de `stratum_api` / `stratum_v1_task`), manutenção do registro de `New Work Dequeued <job_id>` e `Block height %d` em `INFO`, e exposição contínua dos dados de coinbase via UI/API.
- **Micro-alocações por linha de log em `log_buffer_vprintf()` (`malloc`/`free`)** → Mitigado por alocação de curto ciclo de vida fora de seções críticas e fallback explícito para impressão ainda sob `s_log_mutex` caso `malloc` retorne `NULL`, além de drástica redução na frequência de linhas extensas.
- **Possibilidade de `ESP_LOGW` no retry I2C acontecer exatamente em surto de erros repetitivos no barramento** → Como `I2C_RETRY_COUNT = 3` com delay de `10 ms` por tentativa e `POLL_RATE = 100 ms`, o teto natural em falha total já cai no `ESP_LOGE` fatal existente; ainda assim, a linha de retry só dispara em erro real do driver I2C (espera-se zero ocorrências em operação estável).
