# Spec Delta

## ADDED Requirements

### Requirement: Alocação da stack da task LVGL em SRAM interna
Em `display_init()` (`main/display.c`), o sistema SHALL configurar `lvgl_cfg.task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT` (preservando `lvgl_cfg.task_affinity = 0`), garantindo que buffers de stack passados ao driver `esp_lcd_panel_io_i2c` residam em SRAM interna enquanto a SRAM interna livre permanece acima de `85 KB`.

#### Scenario: Atualização do display OLED durante escrita em NVS ou Flash
- **WHEN** `taskLVGL` no Core 0 envia comandos ou quadros ao controlador OLED via `esp_lcd_panel_io_i2c` com `CONFIG_I2C_ISR_IRAM_SAFE=y` simultaneamente a `nvs_commit()` ou operação OTA
- **THEN** a stack de `taskLVGL` reside em SRAM interna, impedindo que o ISR IRAM-safe do `I2C0` acesse regiões em PSRAM com o cache desabilitado

#### Scenario: Preservação de folga mínima de SRAM interna após boot completo
- **WHEN** o firmware inicializa todas as tasks com `taskLVGL` alocada em SRAM interna
- **THEN** a SRAM interna livre em operação normal (`freeHeapInternal`) permanece acima de `85 KB` (meta operacional `>= 110 KB`) sem regressão nas demais tasks alocadas em PSRAM
