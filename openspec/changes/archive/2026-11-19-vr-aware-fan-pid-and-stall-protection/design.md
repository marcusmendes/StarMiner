# Design

## Context

Na implementação atual da Fase 3, `handle_thermal_state_machine()` (`main/tasks/power_management_task.c`) protege tanto o `BM1370` (`>= 68 °C`) quanto o `TPS546D24A` (`>= 90 °C`) com *Soft-Throttling*, porém `FAN_CONTROLLER_task` (`main/tasks/fan_controller_task.c`) calcula a entrada do PID apenas com `chip_temp_avg` / `chip_temp2_avg` e nunca inspeciona `fan_rpm` para detectar falha mecânica de ventilação (veja `proposal.md`).

## Goals / Non-Goals

**Goals:**
- Evitar saltos abruptos de `40% -> 100%` de ventoinha e cortes desnecessários de clock causados por aquecimento isolado do `TPS546D24A` acima de `82–89 °C`.
- Reduzir imediatamente a carga térmica do ASIC via *Soft-Throttling* em RAM e tentar *kickstart* em `100%` PWM dentro de `5 s` após travamento real do rotor (`0 RPM`).
- Manter toda a aritmética de composição térmica em funções puras dentro de `components/asic/power_control_math.c` para cobertura via Unity/QEMU.

**Non-Goals:**
- Alterar os ganhos `PID_P`, `PID_I`, `PID_D` ou a taxa de amostragem de `100 ms` do `FAN_CONTROLLER_task`.
- Criar novas chaves na NVS ou novos campos na API HTTP nesta change.

## Decisions

### Decision 1: Temperatura virtual combinada no PID (`max(asic, vr - 22.0 °C)`)
- **Escolha:** Extrair `pcm_compute_pid_effective_temp(float chip1_temp, float chip2_temp, float vr_temp)` em `components/asic/power_control_math.c` com constante `PCM_VR_TO_ASIC_TEMP_OFFSET_C = 22.0f` (`90.0 °C - 68.0 °C`).
  - Considerar apenas leituras válidas (`!isnan(t) && t > 0.0f`).
  - Se `vr_temp` for válido (`> 0.0f`, o que exclui automaticamente placas sem `TPS546` onde `Power_get_vreg_temp()` retorna `0.0f`), calcular `vr_equiv = vr_temp - 22.0f` e retornar `fmaxf(asic_max, vr_equiv)`.
  - Em `fan_controller_task.c`, passar o resultado para o filtro EMA (`filtered_input`) no modo `Auto` sempre que pelo menos uma temperatura (ASIC ou VR) for válida.
- **Alternativa considerada:** Um segundo controlador PID independente para `vr_temp` usando `max(pid_asic, pid_vr)`. Rejeitada porque o ganho integral/derivativo combinado em duas instâncias de `PIDController` dobra estado e risco de oscilação quando ambas as fontes aquecem juntas; mapear `vr_temp` para o domínio equivalente do ASIC antes do filtro EMA preserva um único laço PID estável.

### Decision 2: Detector de *Fan Stall* (`0 RPM` por `5 s`) acoplado ao *Soft-Throttling*
- **Escolha:** Adicionar `bool fan_stall;` em `PowerManagementModule` (`main/tasks/power_management_task.h`). Em `fan_controller_task.c` (ciclo de `100 ms`), quando houver controlador de fan com tacômetro (`EMC2101 || EMC2103 || EMC2302`), `ASIC_initalized` estiver ativo, `!mining_paused`, `!pools_unavailable`, `!overheat_mode`, `!SELF_TEST_MODULE.is_active`, e o comando atual `fan_perc >= 25.0f`:
  - Se `fan_rpm == 0`, incrementar `stall_cycles` até `50` (`5 s`) e então definir `power_management->fan_stall = true` e comandar `100.0f` (`"Fan Stall"`).
  - Assim que `fan_rpm > 0` (ou se as condições de armação deixarem de valer), zerar `stall_cycles` e limpar `power_management->fan_stall = false`.
  - Em `handle_thermal_state_machine()` (`main/tasks/power_management_task.c`), incluir `power_management->fan_stall` como gatilho da Zona 3 (*Soft-Throttling*), forçando `throttle_active = true` e aplicando os passos normais de redução (`-12.5 MHz` / `-10 mV` a cada `10 s`) enquanto o *stall* persistir, e bloqueando a recuperação da Zona 4 enquanto `fan_stall` estiver ativo.
- **Alternativa considerada:** Disparar `hardware_fault = true` e parar a mineração imediatamente ao ver `0 RPM`. Rejeitada porque um *kickstart* em `100%` PWM frequentemente destrava ventoinhas presas por poeira ou partida lenta sem exigir reinício manual; além disso, se a temperatura continuar subindo, a Zona 1 (`75 °C` / `105 °C`) ainda garante parada crítica de segurança.

## Risks / Trade-offs

- **Risco:** Usuário instalando ventoinha de 2/3 fios sem fio de tacômetro (sempre `fan_rpm == 0`) ativaria `fan_stall` após `5 s`. → **Mitigação:** Em vez de travar o dispositivo em `hardware_fault` permanente, `fan_stall` aplica apenas *Soft-Throttling* progressivo e força PWM `100%`, mantendo a placa protegida e operável; além disso, na Bitaxe Gamma (`601a-4`) a ventoinha padrão de fábrica possui 4 pinos com tacômetro ativo.
- **Risco:** Ruído transitório no tacômetro durante mudança rápida de PWM. → **Mitigação:** A exigência de `50` leituras consecutivas (`5 s` contínuos de `0 RPM`) filtra glitches pontuais de I2C.
