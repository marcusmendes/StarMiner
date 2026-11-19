# Spec Delta

## ADDED Requirements

### Requirement: Notificação de limite aprendido ao Auto-Tuner DVFS em disparo de Watchdog
Quando o Watchdog de ASIC executar recuperação em Nível 1 ou Nível 2 durante um perfil DVFS automático (`Eficiência`, `Equilibrado` ou `Performance`), o sistema SHALL atualizar o piso ou teto aprendido do Auto-Tuner (`learned_min_vcore_mv` / `learned_max_freq_mhz`) para impedir que o DVFS reescale posteriormente para o ponto instável.

#### Scenario: Watchdog Nível 2 em perfil Equilibrado ou Performance reduz e trava teto DVFS
- **WHEN** ocorre a `2ª` ou `3ª` falha do watchdog em `< 10 min` enquanto `dvfs_profile` é `2` (`Equilibrado`) ou `3` (`Performance`) acima do piso do perfil
- **THEN** o sistema reduz `-12.5 MHz` diretamente sobre o alvo DVFS (ou via `watchdog_freq_offset_mhz` se já no piso do perfil) e trava `learned_max_freq_mhz` na frequência reduzida antes de reiniciar o ASIC, impedindo o DVFS de subir novamente no ciclo de `60 s` seguinte

#### Scenario: Watchdog Nível 1 em perfil Equilibrado ou Performance interrompe nova escalada
- **WHEN** ocorre a `1ª` falha do watchdog enquanto `dvfs_profile` é `2` (`Equilibrado`) ou `3` (`Performance`)
- **THEN** a `POWER_MANAGEMENT_task` reinicia o ASIC na mesma frequência e trava `learned_max_freq_mhz` na frequência atual para impedir escalada adicional até nova configuração do usuário

#### Scenario: Watchdog Nível 2 em perfil Eficiência eleva o piso aprendido de VCore
- **WHEN** ocorre a `2ª` ou `3ª` falha do watchdog em `< 10 min` enquanto `dvfs_profile` é `1` (`Eficiência`)
- **THEN** o sistema eleva `learned_min_vcore_mv` em `+10 mV` (limitado a `1250 mV`) além do step-down de `-12.5 MHz` do watchdog antes de reiniciar o ASIC
