# i2c-and-tps546-resilience Specification

## Purpose

Garante a eficiência e a resiliência da comunicação PMBus/I2C com o regulador de tensão `TPS546D24A` e demais periféricos do barramento `I2C0`, eliminando transações redundantes, tratando erros de comunicação `STATUS_CML` de forma não-fatal e recuperando o barramento em caso de travamento de linha.

## Requirements

### Requirement: Cache do registrador estático `PMBUS_VOUT_MODE`
O sistema SHALL ler o registrador `PMBUS_VOUT_MODE` (`0x20`) durante a inicialização do `TPS546` e armazená-lo em cache em memória, reutilizando o valor em todas as conversões `ULINEAR16` de leitura e escrita sem emitir novas transações I2C.

#### Scenario: Conversão de tensão de saída com cache inicializado
- **WHEN** o sistema converte `VOUT` entre formato `ULINEAR16` e ponto flutuante após a inicialização do `TPS546`
- **THEN** a conversão utiliza o expoente de `PMBUS_VOUT_MODE` armazenado em cache sem realizar leitura I2C do registrador `0x20`

### Requirement: Leitura única de `VOUT` por ciclo de telemetria de 100 ms
O sistema SHALL realizar apenas uma leitura I2C de `PMBUS_READ_VOUT` no `TPS546` por ciclo de 100 ms da `POWER_MANAGEMENT_task`, reaproveitando a tensão de saída recém-amostrada em `Power_get_output` para o cálculo de `VCORE_get_voltage_mv`.

#### Scenario: Amostragem de potência e tensão de núcleo no ciclo de 100 ms
- **WHEN** a `POWER_MANAGEMENT_task` executa `Power_get_output` seguido de `VCORE_get_voltage_mv` em uma placa com `TPS546`
- **THEN** o valor de `VOUT` lido em `Power_get_output` é reutilizado por `VCORE_get_voltage_mv` sem emitir uma segunda transação `PMBUS_READ_VOUT` no mesmo ciclo

### Requirement: Tratamento não-fatal de `TPS546_STATUS_CML`
O sistema SHALL inspecionar o bit `TPS546_STATUS_CML` em `TPS546_check_status` e, na ausência de falhas elétricas ou térmicas críticas (`OFF | VOUT_OV | IOUT_OC | VIN_UV | TEMP`), ler `PMBUS_STATUS_CML` (`0x7E`), incrementar o contador `cml_error_count`, registrar aviso em log (`ESP_LOGW`) e enviar `PMBUS_CLEAR_FAULTS` (`0x03`) sem acionar `power_fault`.

#### Scenario: Ocorrência isolada de erro de comunicação `STATUS_CML=0x02 COMM`
- **WHEN** `PMBUS_STATUS_WORD` apresenta `TPS546_STATUS_CML` ativo sem bits críticos (`OFF`, `VOUT_OV`, `IOUT_OC`, `VIN_UV`, `TEMP`)
- **THEN** o sistema lê `PMBUS_STATUS_CML`, incrementa `POWER_MANAGEMENT_MODULE.cml_error_count`, emite `ESP_LOGW`, executa `PMBUS_CLEAR_FAULTS` e mantém `power_fault = 0` com a mineração ativa

### Requirement: Reset nativo do barramento I2C entre tentativas de transferência
O sistema SHALL invocar `i2c_master_bus_reset` no barramento `I2C0` sempre que uma transação I2C falhar antes da última tentativa em `i2c_transfer_with_retries`.

#### Scenario: Falha transitória de transferência I2C na primeira tentativa
- **WHEN** `i2c_master_transmit` ou `i2c_master_transmit_receive` retorna erro em uma tentativa que não é a última (`i + 1 < I2C_RETRY_COUNT`)
- **THEN** o sistema executa `i2c_master_bus_reset` antes do intervalo de espera e realiza a próxima tentativa de transferência

### Requirement: Proteção contra divisão por zero na leitura do tacômetro do EMC2101
Na leitura de rotação da ventoinha em `EMC2101_get_fan_speed`, o sistema SHALL validar se a palavra bruta do tacômetro é diferente de `0x0000` e `0xFFFF` antes de executar a divisão para cálculo de RPM, retornando `0` com segurança quando o valor for inválido.

#### Scenario: Glitch I2C ou registrador de tacômetro zerado durante operação contínua
- **WHEN** os registradores de tacômetro do `EMC2101` retornam `0x0000` ou `0xFFFF` em uma leitura I2C
- **THEN** `EMC2101_get_fan_speed` retorna `0` RPM imediatamente sem executar divisão por zero e sem causar reinício por exceção (`ESP_RST_PANIC`)

### Requirement: Proteção IRAM-safe da interrupção I2C0 sob atividade de cache PSRAM
A configuração do firmware (`sdkconfig.defaults` e `sdkconfig`) SHALL habilitar `CONFIG_I2C_ISR_IRAM_SAFE=y` para manter o atendimento de interrupção do controlador `I2C0` seguro em IRAM durante operações de cache ou sincronização com PSRAM (`esp_cache_msync`).

#### Scenario: Transação I2C0 simultânea a sincronização de cache PSRAM
- **WHEN** `POWER_MANAGEMENT_task` ou `FAN_CONTROLLER_task` executa transações `PMBus`/`I2C0` simultaneamente a chamadas `esp_cache_msync()` ou acesso concorrente à PSRAM no outro núcleo
- **THEN** o manipulador de interrupção do driver `i2c_master` e suas rotinas críticas residem em IRAM (`CONFIG_I2C_ISR_IRAM_SAFE=y`), prevenindo latência adicional de cache no barramento `I2C0`

### Requirement: Observabilidade de retries transitórios e bus reset no barramento I2C0
Em `i2c_transfer_with_retries()` (`main/i2c_bitaxe.c`), o sistema SHALL registrar aviso em log (`ESP_LOGW`) identificando dispositivo (`device_tag` e endereço I2C), número da tentativa e erro sempre que uma transferência falhar antes da última tentativa e acionar `i2c_master_bus_reset()`.

#### Scenario: Falha na primeira tentativa recuperada após bus reset
- **WHEN** `i2c_master_transmit` ou `i2c_master_transmit_receive` falha na tentativa `i` com `i + 1 < I2C_RETRY_COUNT` e a tentativa seguinte obtém `ESP_OK`
- **THEN** o sistema registra em `ESP_LOGW` qual dispositivo (`device_tag` e endereço), tentativa e código de erro dispararam `i2c_master_bus_reset()`, eliminando recuperações transitórias silenciosas

### Requirement: Staging de buffers de transferência I2C em SRAM interna
Em `i2c_transfer_with_retries()` (`main/i2c_bitaxe.c`), o sistema SHALL copiar payloads de escrita e receber leituras exclusivamente através de buffers em SRAM interna (`DRAM_ATTR` estático para transferências `<= 64 B` sob mutex dedicado, ou alocação temporária `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` para `> 64 B`, retornando `ESP_ERR_NO_MEM` em falha), proibindo passar ponteiros em PSRAM ao driver `i2c_master`.

#### Scenario: Chamada de leitura ou escrita I2C originada de task com stack em PSRAM
- **WHEN** `POWER_MANAGEMENT_task`, `FAN_CONTROLLER_task` ou outra task com stack em PSRAM executa transação `<= 64 B` via `i2c_bitaxe_register_*()` simultaneamente à desabilitação de cache por escrita em Flash
- **THEN** `i2c_master_transmit` e `i2c_master_transmit_receive` operam sobre os buffers `DRAM_ATTR` em SRAM interna e o payload recebido é copiado de volta ao buffer do chamador somente após a conclusão da transferência, prevenindo *Cache disabled* panic

#### Scenario: Transferência I2C acima do buffer estático de staging
- **WHEN** um chamador solicita escrita ou leitura com comprimento superior a `64 B`
- **THEN** o sistema aloca temporariamente buffers em `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, executa o ciclo de tentativas com `i2c_master_bus_reset()` preservado em falhas transitórias e libera os buffers internos antes de retornar (ou retorna `ESP_ERR_NO_MEM` se a alocação interna falhar)
