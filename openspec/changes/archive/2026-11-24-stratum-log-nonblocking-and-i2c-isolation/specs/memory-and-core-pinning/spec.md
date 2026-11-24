# Spec Delta

## ADDED Requirements

### Requirement: Afinidade obrigatória da task LVGL ao Core 0
Em `display_init()` (`main/display.c`), o sistema SHALL configurar explicitamente `lvgl_cfg.task_affinity = 0` antes de chamar `lvgl_port_init(&lvgl_cfg)`, mantendo `taskLVGL` e o tráfego I2C do display restritos ao Core 0.

#### Scenario: Inicialização do display e atualização da UI sob carga no Core 0
- **WHEN** `display_init()` inicializa o `esp_lvgl_port` e o Core 0 enfrenta carga transitória de rede ou Stratum
- **THEN** `taskLVGL` permanece fixada no Core 0 com as demais tasks de sistema e barramento `I2C0`, impedindo migração da task de display para o Core 1 de mineração
