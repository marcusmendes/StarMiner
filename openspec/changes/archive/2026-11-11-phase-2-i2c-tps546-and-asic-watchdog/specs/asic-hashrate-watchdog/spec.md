# Spec Delta: asic-hashrate-watchdog

## Purpose

Detecta travamentos totais de hashrate (`0 GH/s` por `60 s`) ou congelamento assimétrico de domínios (`< 10%` da média dos demais por `45 s`) no ASIC `BM1370` e executa recuperação autônoma escalonada em 3 níveis dentro da `POWER_MANAGEMENT_task`.

## ADDED Requirements

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
