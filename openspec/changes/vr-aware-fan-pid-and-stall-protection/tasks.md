# Tasks

## 1. Composição Térmica Pura e Cobertura Unitária (`components/asic`)

- [ ] 1.1 Adicionar `pcm_compute_pid_effective_temp(float chip1_temp, float chip2_temp, float vr_temp)` com `PCM_VR_TO_ASIC_TEMP_OFFSET_C = 22.0f` em `components/asic/include/power_control_math.h` e `components/asic/power_control_math.c`, verificando compilação limpa.
- [ ] 1.2 Adicionar casos `TEST_CASE` em `components/asic/test/test_power_control.c` cobrindo ASIC dominante, VR quente (`86 °C -> 64 °C`), `vr_temp <= 0` / `NaN` e `chip1/chip2` inválidos, executando `./tools/run_qemu_tests.sh` (ou build de `test-ci/`) com 0 falhas.

## 2. Integração no Controle de Ventoinha e Máquina Térmica (`main/tasks`)

- [ ] 2.1 Atualizar `FAN_CONTROLLER_task()` (`main/tasks/fan_controller_task.c`) para alimentar o filtro EMA e o PID com `pcm_compute_pid_effective_temp(chip_temp_avg, chip_temp2_avg, vr_temp)` e implementar o detector de *Fan Stall* (`fan_perc >= 25.0f && fan_rpm == 0` por `50` ciclos de `100 ms`), sinalizando `power_management->fan_stall` e forçando `100%` PWM; validar que logs e `fan_perc` reagem corretamente ao aquecimento simulado de VR e a `0 RPM`.
- [ ] 2.2 Adicionar `bool fan_stall;` em `PowerManagementModule` (`main/tasks/power_management_task.h`) e integrá-lo à Zona 3 (*Soft-Throttling*) e guarda da Zona 4 (*Hysteresis Recovery*) em `handle_thermal_state_machine()` (`main/tasks/power_management_task.c`), verificando que `throttle_active` sobe quando `fan_stall` é verdadeiro e recupera normalmente após retorno de `fan_rpm > 0`.

## 3. Verificação Final

- [ ] 3.1 Executar `idf.py build` na raiz do projeto e a suíte de testes em `test-ci/`, confirmando compilação e testes sem erros.
