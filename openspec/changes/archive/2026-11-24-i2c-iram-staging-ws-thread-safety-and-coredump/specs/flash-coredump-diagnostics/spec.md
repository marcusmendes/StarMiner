# Spec Delta

## Purpose

Habilita a persistência pós-morte de coredumps em formato ELF na Flash interna sem alterar a tabela de partições e expõe o diagnóstico resumido do último crash no `log_buffer` durante o boot.

## ADDED Requirements

### Requirement: Habilitação de coredump ELF em Flash sem alterar o particionamento
A configuração do firmware (`sdkconfig.defaults` e `sdkconfig`) SHALL habilitar `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH=y`, `CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF=y`, `CONFIG_ESP_COREDUMP_CHECKSUM_CRC32=y`, `CONFIG_ESP_COREDUMP_CHECK_BOOT=y`, `CONFIG_ESP_COREDUMP_MAX_TASKS_NUM=32` e `CONFIG_ESP_COREDUMP_STACK_SIZE=2048`, reutilizando a partição `coredump` (`64K`) de `partitions.csv` sem modificar offsets ou tamanhos existentes.

#### Scenario: Compilação e gravação preservando layout OTA existente
- **WHEN** o projeto é reconfigurado (`idf.py reconfigure`) e compilado (`idf.py build`)
- **THEN** `partitions.csv` permanece inalterado (`coredump` de `64K`), o binário vincula o suporte de coredump em Flash com stack dedicada de `2048 B` em DRAM interna e o tamanho máximo de tarefas (`32`) respeita a capacidade da partição em atualizações OTA

### Requirement: Registro automático do resumo pós-morte no boot via log_buffer
Logo após `log_buffer_init()` em `app_main()` (`main/main.c`), quando o coredump em Flash estiver habilitado, o sistema SHALL verificar `esp_core_dump_image_check()` e consultar `esp_core_dump_get_summary()` (alocado no heap) para registrar no `log_buffer` o nome da task (`exc_task`), `exc_pc`, causa da exceção e endereços de *backtrace*, mantendo o dump intacto na Flash.

#### Scenario: Boot imediatamente após reinício por ESP_RST_PANIC com imagem válida
- **WHEN** o dispositivo reinicia com `esp_reset_reason() == ESP_RST_PANIC` e `esp_core_dump_image_check()` retorna `ESP_OK`
- **THEN** `app_main()` grava no `log_buffer` em nível `ESP_LOGE` os detalhes de `esp_core_dump_get_summary()` (incluindo `exc_task`, `exc_pc` e *backtrace*) preservando a imagem na partição `coredump` para extração completa opcional via `espcoredump.py`

#### Scenario: Boot normal sem coredump armazenado na Flash
- **WHEN** o dispositivo inicia e `esp_core_dump_image_check()` indica ausência de imagem válida na partição `coredump`
- **THEN** o sistema conclui a inicialização normalmente sem emitir erro de falha de leitura nem bloquear a subida das tasks de mineração e rede
