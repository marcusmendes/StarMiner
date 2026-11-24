# Spec Delta

## ADDED Requirements

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
