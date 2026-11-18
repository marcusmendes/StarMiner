# Tasks

## 1. Extração de Aritmética Pura e Testes Unity em C (`components/asic`)

- [ ] 1.1 Criar `components/asic/include/power_control_math.h` e `components/asic/power_control_math.c` (registrando o fonte em `components/asic/CMakeLists.txt`) com as funções puras sem dependências de FreeRTOS/hardware (`pcm_clamp_vcore_mv`, `pcm_get_dvfs_freq_bounds`, `pcm_compute_dvfs_base_freq`, `pcm_compute_effective_freq`, `pcm_compute_effective_vcore`, `pcm_step_vdroop_offset_mv`) e reutilizá-las em `main/tasks/power_management_task.c`, verificando que ambos compilam sem warnings.
- [ ] 1.2 Criar `components/asic/test/test_power_control.c` com casos `TEST_CASE` validando clamping de `VCore` (`1000–1250 mV`), piso de frequência (`400.0 MHz`), envelopes DVFS (`0..3`) e passo/teto de *Vdroop* (`+5 mV` até `+20 mV`, bloqueio acima de `32.0 W`), executando `./tools/run_qemu_tests.sh` (ou `idf.py build` em `test-ci/` quando o ambiente ESP-IDF/QEMU estiver ativo) com 0 falhas.

## 2. Testes Unitários dos Componentes `AxeOS` (`Karma / Jasmine`)

- [ ] 2.1 Expandir `main/http_server/axe-os/src/app/components/edit/edit.component.spec.ts` para testar inicialização do formulário com `targetFrequency`/`targetVoltage` distintos de `frequency`/`coreVoltage` e salvamento de `dvfsProfile` com `isRestartRequired === false`, verificando a passagem com `npm run test:ci` em `main/http_server/axe-os`.
- [ ] 2.2 Expandir `main/http_server/axe-os/src/app/components/home/home.component.spec.ts` para testar adição/remoção da mensagem `THERMAL_THROTTLE` conforme `throttleActive` (`1`/`0`) e ausência de `FREQUENCY_LOW` quando `targetFrequency >= 400`, verificando a passagem com `npm run test:ci`.
- [ ] 2.3 Expandir `main/http_server/axe-os/src/app/components/system/system.component.spec.ts` para testar a presença das linhas `DVFS Profile`, `ASIC Watchdog Resets` e `PMBus CML Recoveries` em `getSystemRows()`, executando a suíte completa `npm run test:ci` em `main/http_server/axe-os` com `0` falhas.
