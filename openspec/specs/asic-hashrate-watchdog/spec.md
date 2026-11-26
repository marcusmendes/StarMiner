# asic-hashrate-watchdog Specification

## Purpose

Detecta travamentos totais de hashrate (`0 GH/s` por `60 s`) ou congelamento assimétrico de domínios (`< 10%` da média dos demais por `45 s`) no ASIC `BM1370` e executa recuperação autônoma escalonada em 3 níveis dentro da `POWER_MANAGEMENT_task`.

## Requirements

### Requirement: Condições de armação e janela de carência do Watchdog de ASIC
O sistema SHALL armar a detecção do watchdog de ASIC apenas quando o ASIC estiver inicializado (`ASIC_initalized == true`), com trabalho Stratum ativo (`SYSTEM_MODULE.work_received > 0`), sem pausa, sem indisponibilidade de pools (`!pools_unavailable`), sem falha térmica/elétrica (`overheat_mode == 0`, `power_fault == 0`, `!hardware_fault`), fora de self-test e após decorridos `30 s` de carência desde a última inicialização do ASIC ou transição de frequência.

#### Scenario: Período de carência logo após inicialização ou mudança de clock
- **WHEN** o ASIC acabou de ser inicializado ou teve sua frequência alterada há menos de `30 s`
- **THEN** o monitor de hashrate mantém os contadores de falha zerados e não dispara o gatilho do watchdog

### Requirement: Disparo por hashrate total zero ou domínio assimétrico congelado
Quando armado, o sistema SHALL sinalizar `asic_watchdog_trigger = true` caso o hashrate total permaneça igual a `0.0 GH/s` por `60 s` consecutivos OU caso qualquer domínio de hash do ASIC apresente valor inferior a `10%` da média dos demais domínios por `45 s` consecutivos.

#### Scenario: Hashrate total em 0 GH/s por 60 segundos consecutivos
- **WHEN** o watchdog está armado e `current_hashrate == 0.0` persiste continuamente por `60 s`
- **THEN** o monitor sinaliza `POWER_MANAGEMENT_MODULE.asic_watchdog_trigger = true` e reinicia os acumuladores de detecção

#### Scenario: Um dos 4 domínios do BM1370 congelado abaixo de 10% por 45 segundos
- **WHEN** o watchdog está armado e um domínio apresenta hashrate `< 10%` da média dos outros 3 domínios continuamente por `45 s`
- **THEN** o monitor sinaliza `POWER_MANAGEMENT_MODULE.asic_watchdog_trigger = true` para recuperação do ASIC

### Requirement: Recuperação síncrona escalonada em 3 níveis na `POWER_MANAGEMENT_task`
Ao receber `asic_watchdog_trigger == true`, a `POWER_MANAGEMENT_task` SHALL executar recuperação síncrona escalonada em 3 níveis conforme a recorrência em janela de `10 min`: re-init suave no Nível 1 (`1ª` falha), re-init com redução de `-12.5 MHz` em RAM (piso `400 MHz`, sem alterar a NVS) no Nível 2 (`2ª` e `3ª` falhas), e `esp_restart()` no Nível 3 (`4ª` falha consecutiva).

#### Scenario: Primeira falha recuperada em Nível 1
- **WHEN** `asic_watchdog_trigger` é processado pela primeira vez (ou após `10 min` de operação estável)
- **THEN** a `POWER_MANAGEMENT_task` executa `mining_stop()`, aguarda `2 s`, executa `mining_start()` na mesma frequência alvo, preserva a sessão Wi-Fi/Stratum e incrementa `watchdog_resets`

#### Scenario: Segunda ou terceira falha dentro de 10 minutos (Nível 2 com step-down em RAM)
- **WHEN** um novo disparo do watchdog ocorre em menos de `10 min` após a falha anterior (2ª ou 3ª ocorrência consecutiva)
- **THEN** a `POWER_MANAGEMENT_task` reduz a frequência ativa em RAM em `-12.5 MHz` (limitada ao piso de `400 MHz`, sem sobrescrever a NVS), executa `mining_stop()`, aguarda `2 s`, executa `mining_start()` na frequência reduzida e incrementa `watchdog_resets`

#### Scenario: Quarta falha consecutiva sem estabilização (Nível 3)
- **WHEN** ocorre a 4ª falha consecutiva do watchdog dentro da janela de `< 10 min`
- **THEN** a `POWER_MANAGEMENT_task` registra erro crítico no log e invoca `esp_restart()`

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

### Requirement: Detecção precoce de degradação assimétrica por domínio via EWMA e CUSUM
Quando armado sob as mesmas guardas e carência de `30 s` do watchdog de ASIC (`hash_domains > 1`), `hashrate_monitor_task` SHALL calcular a razão de cada domínio frente à média dos demais, filtrar via `EWMA` (`alpha = 0.20`) e acumular `CUSUM` unilateral (`S >= h`), sinalizando `pre_watchdog_trigger = true` ao cruzar `h` e reiniciando o estado após disparo ou desarmação.

#### Scenario: Ruído instantâneo de 1 segundo em domínio não dispara pré-watchdog
- **WHEN** um único domínio oscila brevemente para `~0,75–0,80` da média dos demais por `1–2 s` e retorna ao normal com o filtro armado
- **THEN** o acumulador `CUSUM` permanece abaixo de `h` e `pre_watchdog_trigger` continua `false`

#### Scenario: Queda sustentada de domínio dispara pré-watchdog antes do congelamento de 45 segundos
- **WHEN** um domínio cai de forma sustentada para `~0,45–0,60` da média dos demais por ~`10–15 s` com o monitor armado
- **THEN** o acumulador `CUSUM` atinge `S >= h`, define `pre_watchdog_trigger = true`, registra `pre_watchdog_domain`, emite `ESP_LOGW` e reinicia o estado do filtro

#### Scenario: Período de carência ou desarmação reinicia estado CUSUM
- **WHEN** `!watchdog_armed` devido a inicialização do ASIC, mudança de frequência há menos de `30 s`, pausa ou falha elétrica/térmica
- **THEN** todos os domínios têm `ewma_ratio` reiniciado para `1.0` e `cusum` zerado para `0.0`

### Requirement: Recuperação preventiva a quente sem parada do ASIC e respeito aos perfis DVFS
Ao receber `pre_watchdog_trigger = true` fora de *overheat*, *throttling* ou falha e após *cooldown* de `60 s`, `POWER_MANAGEMENT_task` SHALL aplicar alívio em RAM sem invocar `mining_stop()` (`+5 mV` se o perfil permitir ajuste de tensão com `power < 30.5 W`, `vr_temp < 84.0 °C` e `VCore + 5 <= 1250 mV`; caso contrário `-12.5 MHz` até o piso de `400 MHz`), preservando o watchdog em 3 níveis.

#### Scenario: Alívio a quente por tensão em perfil com ajuste de VCore e folga elétrica
- **WHEN** `pre_watchdog_trigger` chega com `dvfs_profile` em `1` (`Efficiency`) ou `3` (`Performance`), `power < 30.5 W`, `vr_temp < 84.0 °C` e `effective_vcore <= 1245 mV`
- **THEN** o sistema incrementa `VCore` em `+5 mV` em RAM sem executar `mining_stop()`, incrementa `pre_watchdog_recoveries` e adia o próximo passo DVFS em `60 s`

#### Scenario: Alívio a quente por frequência em perfil Equilibrado ou sem margem de tensão
- **WHEN** `pre_watchdog_trigger` chega com `dvfs_profile` em `2` (`Balanced`) ou `0` (`Manual`), ou sem margem elétrica/térmica para `+5 mV`
- **THEN** o sistema reduz a frequência ativa em RAM em `-12.5 MHz` (limitada ao piso de `400.0 MHz`) sem alterar a NVS nem executar `mining_stop()`, travando `learned_max_freq_mhz` em `Balanced` e `Performance`

#### Scenario: Cooldown de 60 segundos impede oscilações consecutivas
- **WHEN** um novo `pre_watchdog_trigger` é sinalizado menos de `60 s` após o último alívio preventivo
- **THEN** `POWER_MANAGEMENT_task` limpa a flag sem aplicar novo degrau, permitindo que o watchdog de 3 níveis atue caso o domínio congele integralmente
