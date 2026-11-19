# Proposal

## Why

Atualmente, os três perfis automáticos de DVFS (`1 = Eficiência`, `2 = Equilibrado` e `3 = Performance`) em `main/tasks/power_management_task.c` tomam decisões puramente reativas a cada `60 s` sem reter o limite físico do silício de cada Bitaxe Gamma:
1. **Efeito "dente de serra" (*hunting*)**: ao atingir o piso de `VCore` (`Eficiência`) ou o teto de clock (`Equilibrado` e `Performance`), o perfil ultrapassa o ponto estável, sofre aumento de `errorPercentage`, recua um passo e `60 s` depois volta a tentar o mesmo ponto instável indefinidamente.
2. **Cabo de guerra entre DVFS e Watchdog**: se um domínio do `BM1370` congela e o Watchdog Nível 2 reduz `-12.5 MHz` em `watchdog_freq_offset_mhz`, o DVFS ignora o evento e `60 s` depois eleva `dvfs_freq_offset_mhz` novamente.
3. **Zona morta térmica antes do *Soft-Throttling***: entre o teto de subida do DVFS (`64 °C` ASIC / `80 °C` VR no perfil `Equilibrado`) e o início do *Soft-Throttling* (`68 °C` ASIC / `90 °C` VR), o DVFS para de subir mas nunca recua o *overclock* automático quando o ambiente esquenta, esperando o *Soft-Throttling* jogar a ventoinha para `100%`.

## What Changes

- **Motor unificado de aprendizado por chip nos 3 perfis automáticos (`components/asic/power_control_math.c`, `main/tasks/power_management_task.c`)**:
  - **Perfil `1` (`Eficiência`, `500–600 MHz`, erro `<= 0,5%`)**: ao exceder `0,5%` de erro (ou disparo de watchdog), sobe `+10 mV` e trava `learned_min_vcore_mv` nesse piso estável, impedindo novas descidas de `-5 mV` abaixo dele.
  - **Perfil `2` (`Equilibrado`, `525–625 MHz`, erro `<= 0,8%`)**: mantém estritamente a tensão fixa configurada na NVS (`target_voltage` + compensação de *Vdroop*); ao exceder `0,8%` de erro ou receber recuo do watchdog, reduz `-12.5 MHz` e trava `learned_max_freq_mhz` nesse teto estável.
  - **Perfil `3` (`Performance`, `550–675 MHz`, erro `<= 1,0%`)**: aplica aprendizado bidimensional — trava `learned_min_vcore_mv` ao precisar elevar `VCore` (`+10 mV`) e trava `learned_max_freq_mhz` (`-12.5 MHz`) quando o erro exceder `1,0%` no teto de tensão/potência (`1250 mV` ou `>= 31,0 W`) ou em disparo de watchdog.
- **Recuo térmico/elétrico preventivo antes do *Soft-Throttling***:
  - Nos perfis `Equilibrado` (`ASIC >= 65,5 °C` ou `VR >= 84,0 °C`) e `Performance` (`ASIC >= 66,5 °C`, `VR >= 87,0 °C` ou `Power >= 31,2 W`), reduzir `-12.5 MHz` enquanto houver *overclock* positivo de DVFS (`dvfs_freq_offset_mhz > 0`), sem travar `learned_max_freq_mhz`, permitindo retomada gradual até o teto aprendido quando esfriar.
- **Cooperação direta Watchdog <-> DVFS e reset ao alterar NVS**:
  - Notificar o motor DVFS em disparos de Watchdog Nível 1/2 (`handle_asic_watchdog()`) para atualizar `learned_max_freq_mhz` / `learned_min_vcore_mv` sem cabo de guerra.
  - Zerar `learned_max_freq_mhz` e `learned_min_vcore_mv` sempre que `target_frequency`, `target_voltage` ou `dvfs_profile` forem alterados na NVS.

## Capabilities

### New Capabilities
<!-- Nenhuma nova capability necessária; estende o contrato do Auto-Tuner DVFS e sua integração com o Watchdog. -->

### Modified Capabilities
- `vdroop-and-dvfs-autotuner`: Atualizar os requisitos dos perfis `Eficiência`, `Equilibrado` e `Performance` para incluir travamento de piso/teto aprendido por chip, recuo térmico preventivo antes do *Soft-Throttling* e reset de aprendizado em alterações de NVS.
- `asic-hashrate-watchdog`: Adicionar requisito de cooperação com o Auto-Tuner DVFS quando o Watchdog de ASIC dispara durante perfis automáticos.

## Impact

- **Código Afetado**:
  - `components/asic/include/power_control_math.h` / `components/asic/power_control_math.c`
  - `main/tasks/power_management_task.h` / `main/tasks/power_management_task.c`
  - `components/asic/test/test_power_control.c`
- **APIs e Contratos**:
  - Nenhuma alteração de chaves NVS ou endpoints HTTP nesta change; os campos aprendidos permanecem em `PowerManagementModule` até eventual exposição em change dedicada.
