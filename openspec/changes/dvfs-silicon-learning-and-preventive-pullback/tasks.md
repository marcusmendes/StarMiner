# Tasks

## 1. Motor Puro de Aprendizado e Recuo Preventivo (`components/asic`)

- [ ] 1.1 Estender `components/asic/include/power_control_math.h` e `components/asic/power_control_math.c` com os helpers puros de aprendizado por chip e recuo preventivo (`pcm_dvfs_step_efficiency`, `pcm_dvfs_step_balanced`, `pcm_dvfs_step_performance` e cálculo de teto efetivo respeitando `learned_max_freq_mhz` / `learned_min_vcore_mv`), verificando compilação sem warnings.
- [ ] 1.2 Expandir `components/asic/test/test_power_control.c` com casos `TEST_CASE` cobrindo travamento de piso em `Eficiência`, tensão fixa + teto aprendido em `Equilibrado`, aprendizado 2D em `Performance`, recuo térmico preventivo sem travar teto e reset de limites, validando com `./tools/run_qemu_tests.sh` (ou build em `test-ci/`) com 0 falhas.

## 2. Integração na `POWER_MANAGEMENT_task` e Watchdog (`main/tasks`)

- [ ] 2.1 Adicionar `float learned_max_freq_mhz;` e `uint16_t learned_min_vcore_mv;` a `PowerManagementModule` (`main/tasks/power_management_task.h`) e integrá-los em `handle_dvfs_autotuner()` e `reset_dvfs_state()` (`main/tasks/power_management_task.c`), garantindo reset ao alterar `target_frequency`, `target_voltage` ou `dvfs_profile` na NVS e mantendo `dvfs_voltage_offset_mv == 0` no perfil `Equilibrado`.
- [ ] 2.2 Atualizar `handle_asic_watchdog()` (`main/tasks/power_management_task.c`) para atualizar `learned_max_freq_mhz` / `learned_min_vcore_mv` e `dvfs_freq_offset_mhz` nos Níveis 1 e 2 conforme o perfil DVFS ativo, verificando que o DVFS não desfaz reduções do Watchdog após `60 s`.

## 3. Verificação Final

- [ ] 3.1 Executar `idf.py build` na raiz do projeto e a suíte em `test-ci/`, confirmando compilação limpa e 100% dos testes aprovados.
