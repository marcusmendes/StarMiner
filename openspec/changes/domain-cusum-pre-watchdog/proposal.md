# Proposal

## Why

O watchdog atual de ASIC (`asic-hashrate-watchdog`) só atua após um domínio permanecer abaixo de `10%` da média dos demais por `45 s` (ou `0 GH/s` por `60 s`), executando `mining_stop()`, espera de `2 s` e `mining_start()` — o que custa ~`50–70 s` sem minerar por evento. Por outro lado, leituras ao vivo nas 3 placas Bitaxe Gamma 601a mostram variância estatística natural de 1 segundo entre os 4 domínios do `BM1370` (como `r ≈ 0,79–0,81` com `0%` de erro), tornando limiares instantâneos propensos a falsos positivos.

Detectar degradação sustentada de domínio de forma precoce (~`8–15 s`) via filtro `EWMA + CUSUM` e aplicar um alívio preventivo a quente (`+5 mV` ou `-12,5 MHz` em RAM sem parar o ASIC) evita congelamentos completos, protege o *uptime* em mineração solo e cria o contrato rápido de sinalização de instabilidade necessário para a Fase 2 (`adaptive-2d-dvfs-bandit`, `dvfsProfile = 4`).

## What Changes

- **Módulo puro em C para filtro `EWMA + CUSUM` de domínios e decisão de alívio preventivo (`components/asic/include/power_control_math.h`, `components/asic/power_control_math.c`)**:
  - Introduzir `PcmDomainCusumState`, `pcm_domain_cusum_reset()`, `pcm_domain_cusum_update()` e `pcm_pre_watchdog_apply_relief()` com aritmética independente de hardware.
- **Avaliação a `1 Hz` por domínio no Core 1 (`main/tasks/hashrate_monitor_task.c`)**:
  - Calcular a razão relativa de cada domínio frente aos demais, aplicar `EWMA` (`alpha = 0.20`) e `CUSUM` unilateral (`S >= h`), armando estritamente sob as mesmas guardas e carência de `30 s` do watchdog atual e reiniciando o estado quando desarmado ou após disparo.
- **Sinalização e recuperação a quente no Core 0 (`main/tasks/power_management_task.h`, `main/tasks/power_management_task.c`)**:
  - Adicionar campos de contrato em `PowerManagementModule` (`pre_watchdog_trigger`, `pre_watchdog_domain`, `pre_watchdog_cusum_max`, `pre_watchdog_recoveries`, `last_pre_watchdog_action_us`) e executar alívio preventivo em RAM com *cooldown* de `60 s` sem invocar `mining_stop()`, respeitando os *Hard Caps* (`1000–1250 mV`, `400–675 MHz`, `<= 32 W`) e preservando o watchdog em 3 níveis intacto.
- **Cobertura de testes unitários em C (`components/asic/test/test_power_control.c`)**:
  - Validar rejeição de ruído curto de 1 s, detecção de queda sustentada, reset por carência/desarmação e seleção de alívio (`+5 mV` vs `-12,5 MHz`) nos perfis `0..3`.

## Capabilities

### New Capabilities
<!-- Nenhuma nova capability necessária; estende capabilities existentes -->

### Modified Capabilities
- `asic-hashrate-watchdog`: Adiciona detecção precoce de degradação assimétrica por domínio via `EWMA + CUSUM` e recuperação preventiva a quente em RAM (`+5 mV` ou `-12,5 MHz` sem parada do ASIC) com *cooldown* de `60 s` antes do disparo do watchdog de `45 s`.
- `automated-test-coverage`: Adiciona testes unitários Unity em C para o filtro `CUSUM` por domínio e para a lógica de alívio preventivo a quente nos perfis DVFS `0..3`.

## Impact

- **Código Afetado**:
  - `components/asic/include/power_control_math.h`, `components/asic/power_control_math.c`
  - `components/asic/test/test_power_control.c`
  - `main/tasks/hashrate_monitor_task.c`
  - `main/tasks/power_management_task.h`, `main/tasks/power_management_task.c`
- **APIs, NVS, Partições e UI Preservados**:
  - Nenhuma alteração em `main/http_server/openapi.yaml`, endpoints HTTP/WebSocket, `nvs_config`, `PcmRtcDiagnostics`, `partitions.csv` ou frontend `AxeOS` (a exposição de `dvfsProfile = 4` e telemetria correspondente fica reservada para a change da Fase 2).
