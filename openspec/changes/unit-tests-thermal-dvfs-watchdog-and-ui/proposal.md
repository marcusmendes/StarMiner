# Proposal

## Why

As máquinas de estado, cálculos de proteção e integrações de interface introduzidas nas Fases 0 a 4 (`doc/FIRMWARE_IMPROVEMENTS.md` e `axeos-ui-dvfs-telemetry-and-oled-sync`) — incluindo *Hard Caps* (`1000–1250 mV`, piso `400 MHz`), compensação de *Vdroop*, envelopes dos perfis DVFS (`0..3`), *Soft-Throttling* térmico e o tratamento de `targetFrequency`, `dvfsProfile`, `throttleActive`, `watchdogResets` e `cmlErrorCount` no `AxeOS` — ainda não possuem testes unitários dedicados que impeçam regressões em CI (`unittest.yml`).

## What Changes

- **Testes Unitários Frontend Angular (`Karma / Jasmine` em `main/http_server/axe-os/`)**:
  - Expandir `main/http_server/axe-os/src/app/components/edit/edit.component.spec.ts` para verificar:
    - Inicialização de `frequency` e `coreVoltage` a partir de `targetFrequency` e `targetVoltage` quando divergirem dos valores efetivos em RAM (`frequency` e `coreVoltage`).
    - Seleção e envio de `dvfsProfile` (`0..3`) via `updateSystem()` sem marcar `isRestartRequired` como `true`.
  - Expandir `main/http_server/axe-os/src/app/components/home/home.component.spec.ts` para verificar:
    - Exibição do alerta `THERMAL_THROTTLE` quando `throttleActive === 1` e remoção automática quando retorna a `0`.
    - Ausência de falso positivo `FREQUENCY_LOW` quando `frequency < 400` mas `targetFrequency >= 400`.
  - Expandir `main/http_server/axe-os/src/app/components/system/system.component.spec.ts` para verificar as linhas `DVFS Profile`, `ASIC Watchdog Resets` (`watchdogResets`) e `PMBus CML Recoveries` (`cmlErrorCount`) em `getSystemRows()`.
- **Testes Unitários Backend C (`Unity` em `test/` / `test-ci/` via `TEST_COMPONENTS="stratum asic"`)**:
  - Extrair a aritmética pura de controle de tensão/frequência/DVFS/Vdroop de `main/tasks/power_management_task.c` para um módulo compartilhado sem dependências de hardware/FreeRTOS em `components/asic/` (incluído em `components/asic/CMakeLists.txt`).
  - Adicionar suíte `TEST_CASE` em `components/asic/test/` cobrindo: clamping de `VCore` (`1000–1250 mV`), cálculo de frequência efetiva com offsets térmicos/watchdog e piso `400.0 MHz`, limites por perfil DVFS (`0..3`) e ajuste de `vdroop_offset_mv` (deadband `±5 mV`, passo `+5 mV`, teto `+20 mV`).

## Capabilities

### New Capabilities
- `automated-test-coverage`: Cobertura automatizada de testes unitários no CI para a aritmética pura de regulação de potência/frequência/DVFS/Vdroop em C e para os comportamentos críticos de UI do `AxeOS` (`EditComponent`, `HomeComponent`, `SystemComponent`).

### Modified Capabilities
<!-- Nenhuma capability funcional existente em openspec/specs/ é alterada; os testes validam requisitos já especificados. -->

## Impact

- **Código Afetado**:
  - `components/asic/include/` e `components/asic/` (helper puro de cálculo de controle compartilhado com `main/tasks/power_management_task.c`)
  - `components/asic/test/` (novos `TEST_CASE` Unity executáveis via `test-ci` / QEMU)
  - `main/tasks/power_management_task.c` (passa a reutilizar o helper puro compartilhado)
  - `main/http_server/axe-os/src/app/components/edit/edit.component.spec.ts`
  - `main/http_server/axe-os/src/app/components/home/home.component.spec.ts`
  - `main/http_server/axe-os/src/app/components/system/system.component.spec.ts`
- **Dependência de Ordem**:
  - Os testes de UI do `AxeOS` validam comportamentos especificados na change `axeos-ui-dvfs-telemetry-and-oled-sync` e devem ser aplicados após (ou em conjunto com) ela.
