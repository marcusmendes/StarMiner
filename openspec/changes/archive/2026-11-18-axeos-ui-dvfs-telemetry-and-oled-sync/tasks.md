# Tasks

## 1. Preservação de NVS, Seletor DVFS e Overheat Hint no `EditComponent`

- [x] 1.1 Atualizar `main/http_server/axe-os/src/app/components/edit/edit.component.ts` para inicializar `frequency` com `info.targetFrequency ?? info.frequency`, `coreVoltage` com `info.targetVoltage ?? info.coreVoltage`, adicionar o controle `dvfsProfile` (`info.dvfsProfile ?? 0`, `Validators.min(0)`, `Validators.max(3)`) com lista `dvfsProfileOptions` (`0 = Manual`, `1 = Efficiency`, `2 = Balanced`, `3 = Performance`) e incluir `'dvfsProfile'` em `noRestartFields`, verificando com `npm run test:ci` que `EditComponent` compila e passa no `TestBed`.
- [x] 1.2 Atualizar `main/http_server/axe-os/src/app/components/edit/edit.component.html` para adicionar o campo `<p-dropdown>` de `dvfsProfile` e substituir o aviso de `overheat_mode` pela mensagem informando que frequência, tensão e fan da NVS foram preservados, verificando a compilação do template via `npm run build` em `main/http_server/axe-os`.

## 2. Alerta de *Soft-Throttling* e Telemetria de *Vdroop*/DVFS no `HomeComponent`

- [x] 2.1 Atualizar `main/http_server/axe-os/src/app/components/home/home.component.ts` para incluir `'THERMAL_THROTTLE'` em `MessageType`, emitir/remover o aviso `warn` quando `info.throttleActive === 1`, avaliar `'FREQUENCY_LOW'` usando `(info.targetFrequency ?? info.frequency) < 400`, converter `targetVoltage` de mV para V em `startGetLiveData()` e expor helper de label do `dvfsProfile`, verificando com `npm run test:ci` que `HomeComponent` passa sem erros.
- [x] 2.2 Atualizar `main/http_server/axe-os/src/app/components/home/home.component.html` no card **Power** para exibir o perfil DVFS ativo (quando `info.dvfsProfile > 0`), a frequência alvo (`info.targetFrequency`) junto à `actualFrequency` quando divergirem e o indicador `+{{info.vdroopOffsetMv}} mV Vdroop` quando `(info.vdroopOffsetMv ?? 0) > 0`, verificando o build de produção via `npm run build` em `main/http_server/axe-os`.

## 3. Diagnóstico no `SystemComponent` e Mensagem OLED em `main/screen.c`

- [x] 3.1 Atualizar `main/http_server/axe-os/src/app/components/system/system.component.ts` (`getSystemRows()`) para exibir `DVFS Profile`, `ASIC Watchdog Resets` (`watchdogResets`) e `PMBus CML Recoveries` (`cmlErrorCount`), verificando com `npm run test:ci` em `main/http_server/axe-os`.
- [x] 3.2 Atualizar a string de `create_scr_overheat()` em `main/screen.c` para `"Thermal protection active. NVS settings preserved. Clear overheat mode in AxeOS when cool."`, verificando compilação limpa do firmware com `idf.py build` (ou validando a ausência de warnings no trecho alterado caso o toolchain IDF não esteja ativo).
- [x] 3.3 Executar verificação ponta a ponta do frontend (`npm run test:ci` e `npm run build` em `main/http_server/axe-os`) e confirmar que os artefatos gerados de `dist/axe-os` compilaram sem erros de TypeScript ou regressões nos specs existentes.
