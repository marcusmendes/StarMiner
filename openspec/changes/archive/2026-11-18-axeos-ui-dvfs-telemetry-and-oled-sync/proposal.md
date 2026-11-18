# Proposal

## Why

O backend C (`system_api_json.c`) e o schema `openapi.yaml` já expõem `throttleActive`, `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount` e `watchdogResets`, porém nenhum componente visual do frontend Angular (`AxeOS`) foi atualizado para consumi-los. Mais crítico: `EditComponent` (`main/http_server/axe-os/src/app/components/edit/edit.component.ts`) ainda carrega o formulário com `info.frequency` e `info.coreVoltage` (valores dinâmicos em RAM que mudam durante *Soft-Throttling*, compensação de *Vdroop*, *step-down* de watchdog ou Auto-Tuner DVFS) em vez de `info.targetFrequency` e `info.targetVoltage`, fazendo com que salvar qualquer ajuste na tela Settings sobrescreva o alvo do usuário na NVS com um valor transitório da RAM; além disso, a tela OLED de overheat (`main/screen.c`) e o aviso de `overheat_mode` em `edit.component.html` ainda informam incorretamente que frequência, tensão e fan foram resetados.

## What Changes

- **Preservação do Alvo NVS e Seletor de Perfil DVFS no `EditComponent` (`main/http_server/axe-os/src/app/components/edit/`)**:
  - Povoar os controles `frequency` e `coreVoltage` do formulário com `info.targetFrequency ?? info.frequency` e `info.targetVoltage ?? info.coreVoltage`, mantendo os nomes das chaves no payload de `PATCH /api/system`.
  - Adicionar controle de formulário e `<p-dropdown>` para `dvfsProfile` (`0 = Manual`, `1 = Efficiency (500–600 MHz)`, `2 = Balanced (525–625 MHz)`, `3 = Performance (550–675 MHz)`) e incluir `'dvfsProfile'` em `noRestartFields` para que a troca de perfil não exija reinício do dispositivo.
  - Atualizar o texto auxiliar abaixo do botão `Disable Overheat Mode` em `edit.component.html` para informar que frequência, tensão e configuração de fan da NVS foram preservadas e serão retomadas ao limpar o modo de superaquecimento.
- **Alertas de *Soft-Throttling* e Telemetria de *Vdroop*/DVFS no `HomeComponent` (`main/http_server/axe-os/src/app/components/home/`)**:
  - Adicionar `'THERMAL_THROTTLE'` ao `MessageType` e, em `handleSystemMessages()`, exibir um alerta `warn` quando `info.throttleActive === 1` indicando a frequência e tensão efetivas atuais frente aos alvos da NVS.
  - Ajustar a condição de `'FREQUENCY_LOW'` em `handleSystemMessages()` para avaliar `(info.targetFrequency ?? info.frequency) < 400`, evitando falso positivo caso uma redução dinâmica em RAM ou transição de estado reduza momentaneamente a frequência reportada.
  - Exibir no card **Power** (`home.component.html`) o perfil DVFS ativo (quando `dvfsProfile > 0`), a frequência alvo (`targetFrequency`) ao lado de `actualFrequency` quando diferirem, e um indicador de compensação de *Vdroop* (`+{{info.vdroopOffsetMv}} mV` quando `vdroopOffsetMv > 0`), mantendo consistência de unidades (`targetVoltage` convertido de mV para V em `startGetLiveData()`, caso exibido em Volts).
- **Contadores de Resiliência e Perfil DVFS no `SystemComponent` (`main/http_server/axe-os/src/app/components/system/system.component.ts`)**:
  - Exibir em `getSystemRows()` as linhas `DVFS Profile`, `ASIC Watchdog Resets` (`data.info.watchdogResets`) e `PMBus CML Recoveries` (`data.info.cmlErrorCount`).
- **Sincronização da Mensagem OLED de Overheat (`main/screen.c`)**:
  - Atualizar `create_scr_overheat()` para exibir `"Thermal protection active. NVS settings preserved. Clear overheat mode in AxeOS when cool."` em vez da mensagem legada afirmando que energia, frequência e fan foram resetados.

## Capabilities

### New Capabilities
<!-- Nenhuma nova capability necessária; os requisitos estendem capabilities existentes em openspec/specs/. -->

### Modified Capabilities
- `axeos-telemetry-and-dvfs-api`: Estender os requisitos da interface Angular (`AxeOS`) para usar `targetFrequency`/`targetVoltage` no formulário de configurações, permitir selecionar `dvfsProfile` sem restart e exibir `throttleActive`, `vdroopOffsetMv`, `dvfsProfile`, `watchdogResets` e `cmlErrorCount` nas telas Home e System.
- `ram-thermal-throttling-and-safe-overheat`: Estender a especificação para exigir que as mensagens de superaquecimento no display OLED (`SCR_OVERHEAT`) e no formulário do `AxeOS` (`edit.component.html`) reflitam com precisão a preservação das configurações na NVS.

## Impact

- **Código Afetado**:
  - `main/http_server/axe-os/src/app/components/edit/edit.component.ts`
  - `main/http_server/axe-os/src/app/components/edit/edit.component.html`
  - `main/http_server/axe-os/src/app/components/home/home.component.ts`
  - `main/http_server/axe-os/src/app/components/home/home.component.html`
  - `main/http_server/axe-os/src/app/components/system/system.component.ts`
  - `main/screen.c` (`create_scr_overheat`)
- **APIs e Contratos**:
  - Nenhum campo novo no `openapi.yaml` ou `system_api_json.c`; consome exclusivamente os campos já expostos na Fase 4.
