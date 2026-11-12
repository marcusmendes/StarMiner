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
