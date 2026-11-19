# Proposal

## Why

Ao operar uma frota com múltiplas placas Bitaxe Gamma (`601a-4`) em perfil DVFS automático, dois pontos de observabilidade permanecem descobertos:
1. **Perda de histórico e aprendizado no reboot**: um reinício por software (`esp_restart()`, como no Watchdog Nível 3 ou recuperação de rede) apaga completamente da RAM os contadores de resiliência (`watchdogResets`, `cmlErrorCount`), a causa do último disparo de watchdog e os limites aprendidos do silício (`learned_max_freq_mhz` / `learned_min_vcore_mv`), enquanto gravá-los continuamente na NVS causaria desgaste prematuro da Flash.
2. **Página Swarm do AxeOS defasada**: `SwarmComponent` (`main/http_server/axe-os/src/app/components/swarm/`) ainda exibe apenas as métricas legadas do `ESP-Miner`, sem mostrar eficiência (`J/TH`) da frota e de cada placa, perfil DVFS ativo, clock/VCore efetivos (`+Vdroop`), limites aprendidos ou alertas de *Soft-Throttling* e *Watchdog*.

## What Changes

- **Persistência pós-reboot em RAM RTC (`RTC_NOINIT_ATTR`) com CRC32**:
  - Armazenar em `RTC_NOINIT_ATTR` (zero escrita na NVS) uma estrutura protegida por `magic`, *fingerprint* dos alvos da NVS (`target_frequency`, `target_voltage`, `dvfs_profile`) e `CRC32` contendo `watchdog_resets`, `cml_error_count`, `last_watchdog_reason` (`0=None`, `1=Zero Hashrate`, `2=Frozen Domain`, `3=Level 3 Reboot`), `learned_max_freq_mhz` e `learned_min_vcore_mv`.
  - Restaurar esses valores automaticamente após reinícios quentes (`esp_reset_reason() != ESP_RST_POWERON` com `CRC32` e *fingerprint* da NVS válidos) e zerar com segurança em *cold boot* ou mudança de alvo na NVS.
- **Extensão da API HTTP e Contrato OpenAPI**:
  - Expor `learnedMaxFreq` (`number`), `learnedMinVcore` (`integer`) e `lastWatchdogReason` (`integer`, `0..3`) em `GET /api/system/info` (`main/http_server/system_api_json.c`), `main/http_server/openapi.yaml`, modelos gerados e mocks em `system.service.ts`.
- **Dashboard Comparativo de Frota no `SwarmComponent` (`AxeOS`)**:
  - Calcular e exibir a eficiência média da frota (`J/TH` = `Power_W / (Hashrate_GHs / 1000)`) nos cards de resumo e o `J/TH` individual de cada placa nas visões de tabela e grade.
  - Exibir o perfil DVFS ativo (`Manual`, `Efficiency`, `Balanced`, `Performance`), frequência efetiva, `VCore` efetivo com compensação de *Vdroop* (`+X mV`) e tooltip de limite aprendido (`learnedMaxFreq` / `learnedMinVcore`).
  - Atualizar `getDeviceNotification()` para alertar sobre `throttleActive === 1` e `watchdogResets > 0`, e avaliar frequência baixa por `(targetFrequency ?? frequency) < 400`.

## Capabilities

### New Capabilities
<!-- Nenhuma nova capability necessária; estende a telemetria e UI do AxeOS já especificadas. -->

### Modified Capabilities
- `axeos-telemetry-and-dvfs-api`: Adicionar requisitos de persistência em `RTC_NOINIT_ATTR` entre reboots para diagnósticos e aprendizado DVFS, exposição de `learnedMaxFreq`, `learnedMinVcore` e `lastWatchdogReason` na API, e visualização comparativa de frota (`J/TH`, DVFS e alertas) no `SwarmComponent`.

## Impact

- **Código Afetado**:
  - `components/asic/include/power_control_math.h` / `components/asic/power_control_math.c` (lógica pura e CRC32 do registro de diagnóstico RTC)
  - `components/asic/test/test_power_control.c` (testes Unity para serialização/restauração RTC)
  - `main/tasks/hashrate_monitor_task.c`, `main/tasks/power_management_task.h` / `main/tasks/power_management_task.c` (registro da causa do watchdog e sincronização `RTC_NOINIT_ATTR`)
  - `main/http_server/system_api_json.c`, `main/http_server/openapi.yaml`, `main/http_server/axe-os/src/app/generated/*`, `main/http_server/axe-os/src/app/services/system.service.ts`
  - `main/http_server/axe-os/src/app/components/swarm/swarm.component.ts`, `swarm.component.html`, `swarm.component.spec.ts`
- **APIs e Contratos**:
  - Adição retrocompatível de 3 campos opcionais (`learnedMaxFreq`, `learnedMinVcore`, `lastWatchdogReason`) em `SystemInfo`.
