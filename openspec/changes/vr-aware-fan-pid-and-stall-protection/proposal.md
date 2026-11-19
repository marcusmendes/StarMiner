# Proposal

## Why

Atualmente, o controlador PID da ventoinha em `main/tasks/fan_controller_task.c` utiliza exclusivamente a temperatura do ASIC (`chip_temp_avg` / `chip_temp2_avg`) e ignora completamente a temperatura do regulador de tensão `TPS546D24A` (`vr_temp`). Quando o VRM aquece mais rápido que o dissipador do ASIC sob corrente elevada, a ventoinha permanece em rotação baixa até que `vr_temp` atinja `90,0 °C` e dispare abruptamente o *Soft-Throttling* em RAM (jogando o PWM para `100%` e cortando clock). Além disso, caso o rotor da ventoinha trave ou o cabo solte (`0 RPM` com comando PWM `>= 25%`), o firmware não adota nenhuma ação preventiva até o silício alcançar `68 °C` / `75 °C`.

## What Changes

- **PID de Ventoinha Ciente do Regulador (`TPS546`)**:
  - Alimentar o filtro EMA e o controlador PID em `fan_controller_task.c` com uma temperatura virtual combinada `max(temp_asic, temp_vr - 22.0 °C)` (alinhando o limiar térmico do VR de `90 °C` ao do ASIC de `68 °C`), permitindo que a ventoinha acelere suavemente antes de `vr_temp` atingir o limiar de *Soft-Throttling*.
- **Detecção Proativa de *Fan Stall* (`0 RPM`) e Recuperação Automática**:
  - Monitorar `fan_rpm == 0` enquanto o PWM comandado for `>= 25%` durante mineração ativa; após `5 s` contínuos (`50` ciclos de `100 ms`), sinalizar `fan_stall = true`, forçar `100%` PWM (tentativa de *kickstart* do motor) e acionar *Soft-Throttling* preventivo em RAM antes que as temperaturas subam.
  - Limpar `fan_stall` automaticamente assim que `fan_rpm > 0` for restabelecido, permitindo a recuperação gradual por histerese da Zona 4.

## Capabilities

### New Capabilities
<!-- Nenhuma nova capability necessária; estende a proteção térmica em RAM existente. -->

### Modified Capabilities
- `ram-thermal-throttling-and-safe-overheat`: Adicionar requisitos para controle PID ciente da temperatura do regulador `TPS546` e detecção proativa de *Fan Stall* (`0 RPM`) com acionamento de *kickstart* e *Soft-Throttling* preventivo.

## Impact

- **Código Afetado**:
  - `components/asic/include/power_control_math.h` / `components/asic/power_control_math.c` (função pura para temperatura composta do PID)
  - `main/tasks/power_management_task.h` / `main/tasks/power_management_task.c` (integração de `fan_stall` na máquina de estados térmica)
  - `main/tasks/fan_controller_task.c` (temperatura efetiva combinada no PID e detector de `0 RPM`)
  - `components/asic/test/test_power_control.c` (novos casos Unity)
- **APIs e Contratos**:
  - Nenhuma quebra de contrato HTTP/NVS; `fan_stall` aciona `throttleActive = 1` já exposto na API.
