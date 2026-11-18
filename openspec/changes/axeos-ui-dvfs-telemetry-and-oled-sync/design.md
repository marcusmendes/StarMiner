# Design

## Context

Com as Fases 3 e 4 implementadas no backend (`power_management_task.c`, `system_api_json.c`, `openapi.yaml` e mocks de `system.service.ts`), a API `/api/system/info` passou a separar:
- **Alvos persistidos na NVS**: `targetFrequency` (MHz) e `targetVoltage` (mV).
- **Estado efetivo em RAM**: `frequency` / `actualFrequency` (MHz com *Soft-Throttling*, Watchdog *step-down* e DVFS) e `coreVoltage` (mV comandados incluindo compensação de *Vdroop*).
- **Indicadores de operação e resiliência**: `throttleActive` (`0`/`1`), `vdroopOffsetMv`, `dvfsProfile` (`0..3`), `cmlErrorCount` e `watchdogResets`.

Entretanto, `EditComponent`, `HomeComponent`, `SystemComponent` e `main/screen.c` ainda operam sob as suposições anteriores à Fase 3/4 (ver motivação completa em `proposal.md`).

## Goals / Non-Goals

**Goals:**
- Impedir corrupção acidental da NVS pelo `EditComponent` carregando `targetFrequency` e `targetVoltage` no formulário de configurações.
- Habilitar seleção de `dvfsProfile` (`0..3`) via UI (`EditComponent`) sem necessidade de reboot (`noRestartFields`).
- Dar visibilidade no `HomeComponent` e `SystemComponent` a eventos de *Soft-Throttling*, compensação de *Vdroop*, perfil DVFS ativo, resets do watchdog e recuperações `STATUS_CML`.
- Alinhar os textos de overheat no OLED (`main/screen.c`) e no `EditComponent` ao comportamento NVS-preserving da Fase 3.

**Non-Goals:**
- Alterar o contrato JSON ou `openapi.yaml` (todos os campos necessários já existem em `SystemInfo` e `Settings`).
- Modificar a máquina de estados em `power_management_task.c` ou adicionar gráficos históricos novos no `HomeComponent` para offsets de *Vdroop*/DVFS.

## Decisions

### 1. Preservação de NVS no `EditComponent` (`edit.component.ts` / `edit.component.html`)
- **Decisão:** Em `loadDeviceSettings()`, inicializar o `FormGroup` com:
  - `coreVoltage: [info.targetVoltage ?? info.coreVoltage, [Validators.required]]`
  - `frequency: [info.targetFrequency ?? info.frequency, [Validators.required]]`
  - `dvfsProfile: [info.dvfsProfile ?? 0, [Validators.required, Validators.min(0), Validators.max(3)]]`
- **Rationale:** O payload `PATCH /api/system` espera as chaves `frequency`, `coreVoltage` e `dvfsProfile` (definidas em `nvs_config.c` e `Settings` no `openapi.yaml`). Ler `targetFrequency` e `targetVoltage` para inicializar esses campos evita que uma redução térmica temporária em RAM ou um offset de *Vdroop* seja gravado na NVS quando o usuário salva alterações de fan, display ou `dvfsProfile`. O fallback `??` mantém compatibilidade caso um dispositivo remoto no Swarm ainda rode firmware legado sem os campos novos.
- **Alternativa considerada:** Enviar `targetFrequency`/`targetVoltage` em `PATCH /api/system` — descartada porque exigiria alterar o schema `Settings` no backend (`nvs_config.c` e `openapi.yaml`) e quebraria compatibilidade com clientes existentes.
- **Dropdown DVFS e `noRestartFields`:** Adicionar lista fixa de perfis (`Manual`, `Efficiency (500–600 MHz)`, `Balanced (525–625 MHz)`, `Performance (550–675 MHz)`) e incluir `'dvfsProfile'` em `noRestartFields`, já que `POWER_MANAGEMENT_task` lê `NVS_CONFIG_DVFS_PROFILE` a cada ciclo de `100 ms`.

### 2. Alertas e Card Power no `HomeComponent` (`home.component.ts` / `home.component.html`)
- **Decisão:**
  - Adicionar `'THERMAL_THROTTLE'` ao tipo `MessageType` e avaliar `info.throttleActive === 1` em `handleSystemMessages()` exibindo mensagem `warn` com frequência efetiva vs. alvo.
  - Atualizar a condição de `'FREQUENCY_LOW'` para avaliar `(info.targetFrequency || info.frequency) < 400`, evitando falso alarme quando o alvo na NVS é `>= 400 MHz`.
  - No card **Power** (`home.component.html`), manter a leitura principal em `info.actualFrequency` e `info.coreVoltageActual` e adicionar indicadores condicionais compactos:
    - Exibir perfil DVFS ativo (`getDvfsProfileLabel(info.dvfsProfile)`) quando `info.dvfsProfile > 0` e exibir `(Target: {{info.targetFrequency}} MHz)` quando `info.targetFrequency` diferir de `info.actualFrequency`.
    - Exibir `+{{info.vdroopOffsetMv}} mV Vdroop` junto à linha de tensão medida quando `(info.vdroopOffsetMv ?? 0) > 0`.
  - Em `startGetLiveData()`, converter `processed.targetVoltage = (processed.targetVoltage ?? processed.coreVoltage) / 1000` (arredondado para 2 casas decimais) para manter paridade de unidades em Volts dentro do stream `info$` do `HomeComponent`.
- **Alternativa considerada:** Criar um widget GridStack separado apenas para DVFS/Vdroop — descartada por aumentar a complexidade de layout salvo no `localStorage` (`DASHBOARD_LAYOUT_V1`) de usuários existentes.

### 3. Linhas de Diagnóstico no `SystemComponent` (`system.component.ts`)
- **Decisão:** Adicionar em `getSystemRows()` (usando helper local `formatDvfsProfile(profile)`):
  - `DVFS Profile` (`Manual` / `Efficiency` / `Balanced` / `Performance`)
  - `ASIC Watchdog Resets` (`String(data.info.watchdogResets ?? 0)`)
  - `PMBus CML Recoveries` (`String(data.info.cmlErrorCount ?? 0)`)

### 4. Mensagens Coerentes de Overheat (`main/screen.c` e `edit.component.html`)
- **Decisão:** Substituir o texto legado em `create_scr_overheat()` (`main/screen.c#L150`) por `"Thermal protection active. NVS settings preserved. Clear overheat mode in AxeOS when cool."` e atualizar a nota auxiliar em `edit.component.html` para `"Frequency, voltage, and fan settings were preserved in NVS. Adjust targets if needed before resuming."`.

## Risks / Trade-offs

- **[Risk] Firmware legado em dispositivo remoto no Swarm sem `targetFrequency` / `dvfsProfile` / `throttleActive`** → **Mitigação**: Usar nullish coalescing (`??`) e valores padrão seguros (`0`) em todos os templates e métodos TypeScript (`EditComponent`, `HomeComponent`, `SystemComponent`).
- **[Risk] Divisão de `targetVoltage` em `HomeComponent.startGetLiveData()` afetar outro componente** → **Mitigação**: O `HomeComponent` aplica o pipe `map()` localmente em `this.info$`, não modificando o stream compartilhado `LiveDataService.info$` consumido pelo `EditComponent`.
- **[Risk] Testes existentes (`edit.component.spec.ts`, `home.component.spec.ts`, `system.component.spec.ts`) quebrarem se faltarem módulos PrimeNG ou pipes no `TestBed`** → **Mitigação**: Verificar o build e `npm run test:ci` com atenção às dependências declaradas nos `.spec.ts`.
