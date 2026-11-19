# Spec Delta

## ADDED Requirements

### Requirement: Controle PID de Ventoinha Ciente da Temperatura do Regulador (TPS546)
No modo automático de ventoinha (`Auto`), o controlador PID SHALL utilizar a temperatura virtual composta `max(temp_asic, temp_vr - 22.0 °C)` sempre que `vr_temp` for válido (`> 0 °C`), alinhando a resposta de resfriamento ao envelope térmico do regulador de tensão antes do disparo de *Soft-Throttling*.

#### Scenario: Regulador de tensão aquece próximo ao limite enquanto o ASIC permanece frio
- **WHEN** o controlador está em modo automático com `temptarget = 60 °C`, `chip_temp_avg = 58.0 °C` e `vr_temp = 86.0 °C`
- **THEN** a temperatura efetiva entregue ao filtro EMA do PID passa a `64.0 °C` (`86.0 - 22.0`) e o PID acelera suavemente a ventoinha antes que `vr_temp` alcance `90.0 °C`

#### Scenario: Regulador frio ou dispositivo sem sensor TPS546
- **WHEN** `vr_temp = 74.0 °C` com `chip_temp_avg = 60.0 °C` (ou `vr_temp <= 0.0 °C` em placa sem `TPS546`)
- **THEN** a temperatura efetiva do PID permanece igual à maior temperatura válida do ASIC (`60.0 °C`) sem perturbação pela compensação do VRM

### Requirement: Detecção Proativa de Ventoinha Travada (Fan Stall 0 RPM)
Durante mineração ativa com monitoramento de tacômetro habilitado, o sistema SHALL detectar *Fan Stall* quando o PWM comandado permanecer `>= 25%` com `fan_rpm == 0` por `5 s` contínuos, forçando `100%` PWM de *kickstart* e ativando o *Soft-Throttling* preventivo em RAM até o retorno da rotação.

#### Scenario: Rotor travado ou cabo do tacômetro desconectado durante mineração ativa
- **WHEN** o ASIC está ativo (não pausado e com pool disponível), `fan_perc >= 25.0%` e `fan_rpm == 0` persiste continuamente por `5 s`
- **THEN** o sistema ativa `fan_stall`, força a ventoinha a `100%` PWM e inicia o *Soft-Throttling* em RAM (`throttle_active = true`) sem aguardar o ASIC atingir `68.0 °C`

#### Scenario: Recuperação após retomada da rotação da ventoinha
- **WHEN** `fan_stall` está ativo e o sensor passa a reportar `fan_rpm > 0`
- **THEN** o sistema limpa `fan_stall` imediatamente e permite a recuperação gradual por histerese na Zona 4 assim que as temperaturas e potência estiverem seguras
