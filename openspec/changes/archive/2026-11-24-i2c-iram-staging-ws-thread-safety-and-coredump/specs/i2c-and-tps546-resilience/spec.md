# Spec Delta

## ADDED Requirements

### Requirement: Staging de buffers de transferência I2C em SRAM interna
Em `i2c_transfer_with_retries()` (`main/i2c_bitaxe.c`), o sistema SHALL copiar payloads de escrita e receber leituras exclusivamente através de buffers em SRAM interna (`DRAM_ATTR` estático para transferências `<= 64 B` sob mutex dedicado, ou alocação temporária `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` para `> 64 B`, retornando `ESP_ERR_NO_MEM` em falha), proibindo passar ponteiros em PSRAM ao driver `i2c_master`.

#### Scenario: Chamada de leitura ou escrita I2C originada de task com stack em PSRAM
- **WHEN** `POWER_MANAGEMENT_task`, `FAN_CONTROLLER_task` ou outra task com stack em PSRAM executa transação `<= 64 B` via `i2c_bitaxe_register_*()` simultaneamente à desabilitação de cache por escrita em Flash
- **THEN** `i2c_master_transmit` e `i2c_master_transmit_receive` operam sobre os buffers `DRAM_ATTR` em SRAM interna e o payload recebido é copiado de volta ao buffer do chamador somente após a conclusão da transferência, prevenindo *Cache disabled* panic

#### Scenario: Transferência I2C acima do buffer estático de staging
- **WHEN** um chamador solicita escrita ou leitura com comprimento superior a `64 B`
- **THEN** o sistema aloca temporariamente buffers em `MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT`, executa o ciclo de tentativas com `i2c_master_bus_reset()` preservado em falhas transitórias e libera os buffers internos antes de retornar (ou retorna `ESP_ERR_NO_MEM` se a alocação interna falhar)
