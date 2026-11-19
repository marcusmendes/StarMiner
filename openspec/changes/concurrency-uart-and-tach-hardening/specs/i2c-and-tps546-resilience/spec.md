# Spec Delta

## ADDED Requirements

### Requirement: Proteção contra divisão por zero na leitura do tacômetro do EMC2101
Na leitura de rotação da ventoinha em `EMC2101_get_fan_speed`, o sistema SHALL validar se a palavra bruta do tacômetro é diferente de `0x0000` e `0xFFFF` antes de executar a divisão para cálculo de RPM, retornando `0` com segurança quando o valor for inválido.

#### Scenario: Glitch I2C ou registrador de tacômetro zerado durante operação contínua
- **WHEN** os registradores de tacômetro do `EMC2101` retornam `0x0000` ou `0xFFFF` em uma leitura I2C
- **THEN** `EMC2101_get_fan_speed` retorna `0` RPM imediatamente sem executar divisão por zero e sem causar reinício por exceção (`ESP_RST_PANIC`)
