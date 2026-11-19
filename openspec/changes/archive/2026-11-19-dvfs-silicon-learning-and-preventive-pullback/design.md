# Design

## Context

Cada chip `BM1370` apresenta característica própria na *loteria do silício*. Hoje o loop DVFS em `handle_dvfs_autotuner()` (`main/tasks/power_management_task.c`) opera sem memória dos pontos que falharam e sem enxergar `watchdog_freq_offset_mhz` aplicado por `handle_asic_watchdog()`, além de manter *overclock* automático ativo na banda térmica intermediária logo abaixo do *Soft-Throttling* (`68 °C` ASIC / `90 °C` VR). Veja `proposal.md` para a motivação completa.

## Goals / Non-Goals

**Goals:**
- Eliminar ciclos repetitivos de subida e erro (*ping-pong*) nos três perfis DVFS automáticos (`Eficiência`, `Equilibrado`, `Performance`).
- Respeitar estritamente a escolha de **Tensão Fixa + Teto Aprendido** no perfil `Equilibrado` (`dvfs_voltage_offset_mv` permanece `0`, com compensação lenta apenas via *Vdroop*).
- Garantir recuo preventivo suave (`-12.5 MHz`) dentro do DVFS antes de acionar *Soft-Throttling* (`100%` fan) quando a temperatura ambiente subir.
- Sincronizar disparos do Watchdog de ASIC (`Nível 1` e `Nível 2`) com os limites aprendidos pelo DVFS.

**Non-Goals:**
- Persistir limites aprendidos entre reboots em NVS ou RAM RTC (coberto pela change `rtc-noinit-diagnostics-and-swarm-fleet-ui`).
- Modificar a curva de compensação lenta de *Vdroop* (`+5..+20 mV`).

## Decisions

### Decision 1: Estado de aprendizado em `PowerManagementModule` e funções puras em `power_control_math`
- **Escolha:** Adicionar `float learned_max_freq_mhz;` (`0.0f` = não travado) e `uint16_t learned_min_vcore_mv;` (`0` = não travado) em `PowerManagementModule` (`main/tasks/power_management_task.h`). Extrair a lógica de transição e limites para funções puras em `components/asic/power_control_math.c` (`pcm_dvfs_effective_freq_ceiling()`, `pcm_dvfs_step_efficiency()`, `pcm_dvfs_step_balanced()`, `pcm_dvfs_step_performance()`), permitindo validação determinística via testes Unity sem hardware físico.
- **Alternativa considerada:** Manter toda a máquina de estados diretamente inline em `power_management_task.c`. Rejeitada porque o projeto padroniza testar regras críticas de DVFS/térmica no runner Unity/QEMU via `power_control_math`.

### Decision 2: Semântica por perfil para aprendizado e recuo preventivo
- **Perfil `1` (`Eficiência`)**:
  - Se `error_percentage > 0.5%`: tenta subir `+10 mV` (`pcm_clamp_vcore_mv`, apenas se `power_valid && power <= 32.0 W`) e atualiza `learned_min_vcore_mv = max(learned_min_vcore_mv, next_mv)`.
  - Se `error_percentage <= 0.5%`: só reduz `-5 mV` se `cur_mv > effective_floor` (onde `effective_floor = max(1000, learned_min_vcore_mv)`).
- **Perfil `2` (`Equilibrado`, tensão fixa da NVS)**:
  - `dvfs_voltage_offset_mv` é sempre `0`.
  - Se `error_percentage > 0.8%`: reduz `-12.5 MHz` (piso `525.0 MHz`) e trava `learned_max_freq_mhz = next_freq` (ou `min(learned_max_freq_mhz, next_freq)`).
  - Else if `chip_temp >= 65.5 °C` ou `vr_temp >= 84.0 °C` e `cur_freq > clamped_target_freq`: aplica recuo térmico preventivo `-12.5 MHz` (até `clamped_target_freq`) **sem** alterar `learned_max_freq_mhz`.
  - Else if `chip_temp < 64.0 °C`, `vr_temp < 80.0 °C` e `power <= 32.0 W`: sobe `+12.5 MHz` respeitando `min(625.0 MHz, learned_max_freq_mhz)`.
- **Perfil `3` (`Performance`, aprendizado 2D)**:
  - Se `error_percentage > 1.0%`: quando `cur_mv < 1250 mV` e `power < 31.0 W`, sobe VCore `+10 mV` e trava `learned_min_vcore_mv = max(learned_min_vcore_mv, next_mv)`; caso contrário, reduz `-12.5 MHz` (piso `550.0 MHz`) e trava `learned_max_freq_mhz = next_freq`.
  - Else if `(chip_temp >= 66.5 °C || vr_temp >= 87.0 °C || power >= 31.2 W)` e `cur_freq > clamped_target_freq`: recuo preventivo `-12.5 MHz` sem travar `learned_max_freq_mhz`.
  - Else if `chip_temp < 68.0 °C`, `vr_temp < 90.0 °C` e `power < 31.0 W`: sobe `+12.5 MHz` respeitando `min(675.0 MHz, learned_max_freq_mhz)`.

### Decision 3: Integração Watchdog (`handle_asic_watchdog`) <-> DVFS
- **Escolha:**
  - **Watchdog Nível 1 (`1ª falha`)**: mantém o comportamento de re-init na mesma frequência (`asic-hashrate-watchdog`), mas se `dvfs_profile` for `Equilibrado` ou `Performance`, trava `learned_max_freq_mhz = cur_freq` (impedindo escalada adicional acima do ponto onde ocorreu a falha até que o usuário mude a configuração).
  - **Watchdog Nível 2 (`2ª/3ª falha < 10 min`)**: se `dvfs_profile` for `Equilibrado` ou `Performance` e `cur_freq > profile_min_freq`, executa o step-down de `-12.5 MHz` diretamente sobre `dvfs_freq_offset_mhz` e trava `learned_max_freq_mhz = next_freq` (em vez de competir com `watchdog_freq_offset_mhz`); se já estiver em `profile_min_freq` ou em `Manual`, usa `watchdog_freq_offset_mhz` normalmente até o piso de `400 MHz`. Se `dvfs_profile == Eficiência`, além do step-down do watchdog, eleva `learned_min_vcore_mv` em `+10 mV` (até `1250 mV`) e aplica `dvfs_voltage_offset_mv` correspondente antes de `mining_start()`.
- **Alternativa considerada:** Reduzir frequência no Nível 1 imediatamente. Rejeitada para preservar o contrato já especificado em `asic-hashrate-watchdog` (Nível 1 tenta re-init suave na mesma frequência antes de reduzir clock no Nível 2).

### Decision 4: Invalidação do aprendizado ao alterar configurações da NVS
- **Escolha:** Na detecção de `freq_changed || volt_changed || profile_changed` em `POWER_MANAGEMENT_task` (`main/tasks/power_management_task.c`), resetar `learned_max_freq_mhz = 0.0f` e `learned_min_vcore_mv = 0` junto com `reset_dvfs_state()`.

## Risks / Trade-offs

- **Risco:** Um pico transitório de erro logo após troca de bloco ou queda momentânea da pool travar `learned_max_freq_mhz` cedo demais. → **Mitigação:** A avaliação de `handle_dvfs_autotuner()` ocorre apenas a cada `60 s` e exige `ASIC_initalized && !overheat_mode && !throttle_active`; além disso, recuos térmicos preventivos (`65,5 °C / 84 °C`) não travam `learned_max_freq_mhz`, e qualquer ajuste em NVS limpa o aprendizado imediatamente.
- **Risco:** `target_frequency` configurada na NVS acima da faixa de `Equilibrado` (ex.: `625 MHz`) impedir o recuo preventivo abaixo de `target_frequency`. → **Mitigação:** Se `target_frequency` já estiver acima da zona térmica segura, a Zona 3 (*Soft-Throttling* em `68 °C / 90 °C`) continua atuando até o piso absoluto de `400 MHz`.
