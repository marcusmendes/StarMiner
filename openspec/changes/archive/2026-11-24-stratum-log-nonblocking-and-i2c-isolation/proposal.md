# Proposal

## Why

A análise dos logs de produção da placa StarMiner Bitaxe Gamma (`601a-4`, uptime `4d08h`, `1.718` ocorrências acumuladas ≈ `16,4/h`) revelou que **100% (7 de 7)** dos avisos `TPS546 non-fatal STATUS_CML=0x02 (word=0x0202)` na amostra de 23 minutos (`7/48` notificações ≈ `14,6%`) acontecem exclusivamente entre **+122 ms e +168 ms** após a chegada de `mining.notify`, mesmo quando `clean_jobs = false` (sem envio de trabalho ao ASIC). A investigação no código confirmou quatro fatores combinados que criam essa janela de falha SMBus no barramento `I2C0`:

1. **Bloqueio síncrono de ~150–168 ms na `UART0` durante `mining.notify`**: `STRATUM_V1_parse()` (`components/stratum/stratum_api.c:585`) despeja integralmente os ~1,1–1,35 KB do JSON recebido via `ESP_LOGI(TAG, "rx: %s", stratum_json)` a `115.200 baud` (~11,52 bytes/ms sem driver por interrupção), seguido por até seis chamadas `ESP_LOGI` em `decode_mining_notification()` (`main/tasks/stratum_v1_task.c:109-165`) e `New Work Dequeued` no Core 1.
2. **Inversão de prioridade via `s_log_mutex` em `log_buffer_vprintf()`**: `main/log_buffer.c:74-120` mantém o mutex recursivo `s_log_mutex` retido durante toda a execução bloqueante de `fputs(output, stdout)`. Quando tasks de maior prioridade (`ASIC_result_task` prio 15 ou `create_jobs_task` prio 10 no Core 1) tentam logar durante o `mining.notify`, a herança de prioridade eleva `stratum_v1_task` (Core 0, prio 5) acima de `POWER_MANAGEMENT_task` (Core 0, prio 10), podendo causar *starvation* no Core 0 e estourar o timeout SMBus (~25 ms) do `TPS546D24A`.
3. **Contenção de cache PSRAM sem `CONFIG_I2C_ISR_IRAM_SAFE=y` e retries silenciosos**: cada linha de log em `ring_write()` (`main/log_buffer.c:43-65`) invoca duas vezes `esp_cache_msync()` sobre PSRAM Octal enquanto todas as stacks de tasks I2C/sistema residem em PSRAM e `# CONFIG_I2C_ISR_IRAM_SAFE is not set` no `sdkconfig`. Falhas transitórias resultantes disparam `i2c_master_bus_reset()` silenciosamente em `i2c_transfer_with_retries()` (`main/i2c_bitaxe.c:49-55`), gerando 9 pulsos extras de `SCL` (`STATUS_CML=0x02`) e um atraso de `I2C_RETRY_DELAY_MS = 10 ms`.
4. **Migração cruzada de `taskLVGL` para o Core 1**: `display_init()` (`main/display.c:94-96`) mantém `lvgl_cfg.task_affinity = -1` (prio 4), permitindo que o escalonador mova `taskLVGL` para o Core 1 durante a saturação do Core 0 no `mining.notify`, disparando transações concorrentes `esp_lcd_panel_io_i2c` no mesmo barramento `I2C0` a `400 kHz`.

## What Changes

- **Redução do volume síncrono de log no Stratum V1 (`components/stratum/stratum_api.c`, `main/tasks/stratum_v1_task.c`)**:
  - Rebaixar `ESP_LOGI(TAG, "rx: %s", stratum_json)` para `ESP_LOGD`, mantendo apenas resumo curto de controle em `ESP_LOGI` (como tamanho/tipo ou `job_id` e `clean_jobs`).
  - Reduzir a rajada repetitiva em `ESP_LOGI` dentro de `decode_mining_notification()`, preservando detalhes verbosos (sinalizações, `Scriptsig` e cada output de coinbase) em `ESP_LOGD` ou emitindo alterações de `block_height` e conjunto de outputs apenas quando houver mudança efetiva.
- **Desacoplamento de `s_log_mutex` da escrita síncrona em `stdout` (`main/log_buffer.c`)**:
  - Reter `s_log_mutex` em `log_buffer_vprintf()` apenas durante a formatação em `s_vprintf_buffer`, a cópia no ring buffer (`ring_write()`) e `websocket_log_notify()`, liberando o mutex antes de invocar `fputs(..., stdout)` sobre um buffer independente.
- **Proteção IRAM-safe e observabilidade do barramento `I2C0` (`sdkconfig.defaults`, `sdkconfig`, `main/i2c_bitaxe.c`)**:
  - Habilitar `CONFIG_I2C_ISR_IRAM_SAFE=y` para manter o ISR do controlador `I2C0` determinístico durante sincronizações de cache/PSRAM.
  - Registrar aviso (`ESP_LOGW`) em `i2c_transfer_with_retries()` identificando o dispositivo (`device_tag`, endereço), a tentativa e o erro sempre que um retry acionar `i2c_master_bus_reset()` (eliminando retries silenciosos).
- **Afinidade explícita de `taskLVGL` ao Core 0 (`main/display.c`)**:
  - Definir `lvgl_cfg.task_affinity = 0` em `display_init()` antes de `lvgl_port_init(&lvgl_cfg)` para isolar todo o tráfego I2C e de UI no Core 0, mantendo o Core 1 dedicado à mineração.

## Capabilities

### New Capabilities
- `stratum-and-console-log-resilience`: Garante que o processamento de mensagens Stratum V1 e o subsistema de buffer de log não bloqueiem a CPU nem mantenham mutexes globais retidos durante E/S síncrona em console serial.

### Modified Capabilities
- `i2c-and-tps546-resilience`: Adiciona proteção `CONFIG_I2C_ISR_IRAM_SAFE=y` para a interrupção do `I2C0` sob operações de cache PSRAM e exige observabilidade explícita de retries transitórios que acionam `i2c_master_bus_reset`.
- `memory-and-core-pinning`: Estende o isolamento de núcleos para fixar explicitamente `taskLVGL` no Core 0 em `display_init()`.

## Impact

- **Código Afetado**:
  - `components/stratum/stratum_api.c` (`STRATUM_V1_parse`, `parse_mining_notify`)
  - `main/tasks/stratum_v1_task.c` (`decode_mining_notification`)
  - `main/log_buffer.c` (`log_buffer_vprintf`)
  - `main/i2c_bitaxe.c` (`i2c_transfer_with_retries`)
  - `main/display.c` (`display_init`)
  - `sdkconfig.defaults` e `sdkconfig` (`CONFIG_I2C_ISR_IRAM_SAFE=y`)
- **APIs, NVS e Compatibilidade**:
  - Nenhuma alteração em contratos HTTP/JSON, `openapi.yaml`, mocks do AxeOS ou chaves NVS.
  - Redução de verbosidade em nível `INFO` para payload JSON bruto do Stratum V1 (payload completo preservado em nível `DEBUG` e WebSocket/ring buffer mantidos íntegros para todas as mensagens emitidas).
