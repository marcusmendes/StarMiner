# Design

## Context

Com o aprendizado por chip do DVFS ativo em múltiplas Bitaxe Gammas (`601a-4`), a perda de estado após um `esp_restart()` oculta a causa raiz de reinícios noturnos e força o Auto-Tuner a reexplorar o limite de silício do zero, enquanto a página **Swarm** do `AxeOS` ainda não agrega métricas de eficiência ou estado DVFS das placas na mesma rede (veja `proposal.md`).

## Goals / Non-Goals

**Goals:**
- Preservar contadores críticos (`watchdogResets`, `cmlErrorCount`, `lastWatchdogReason`) e limites aprendidos (`learned_max_freq_mhz`, `learned_min_vcore_mv`) através de `esp_restart()` com zero desgaste na NVS/Flash.
- Invalidar automaticamente o cache RTC em perda total de energia (`ESP_RST_POWERON`), corrupção de memória (`CRC32`) ou alteração dos alvos configurados pelo usuário na NVS.
- Permitir comparação imediata das 3 Bitaxe Gammas lado a lado na aba **Swarm** do `AxeOS` (eficiência `J/TH`, perfil DVFS, clock/VCore efetivos, *Vdroop* e alertas).

**Non-Goals:**
- Persistência em disco/Flash após desligamento físico da tomada.
- Alteração de configurações em lote (*bulk edit*) pela página Swarm.

## Decisions

### Decision 1: Bloco `RTC_NOINIT_ATTR` validado por Magic, Fingerprint NVS e CRC32
- **Escolha:** Definir em `components/asic/include/power_control_math.h` a struct `PcmRtcDiagnostics` contendo:
  - `uint32_t magic` (`0x53545244`, `"STRD"`)
  - `uint32_t crc32` (calculado sobre todos os demais campos com `crc32 = 0`)
  - `uint16_t target_voltage_mv`, `uint16_t dvfs_profile`, `float target_frequency_mhz` (fingerprint dos alvos NVS)
  - `uint32_t watchdog_resets`, `uint32_t cml_error_count`
  - `uint8_t last_watchdog_reason` (`0=None, 1=Zero Hashrate, 2=Frozen Domain, 3=Level 3 Reboot`)
  - `float learned_max_freq_mhz`, `uint16_t learned_min_vcore_mv`
  Implementar as funções puras `pcm_rtc_diag_seal()` e `pcm_rtc_diag_validate()` em `power_control_math.c` (sem dependência de hardware, totalmente testáveis no runner Unity/QEMU).
  Em `main/tasks/power_management_task.c`, declarar `static RTC_NOINIT_ATTR PcmRtcDiagnostics s_rtc_diag;` e restaurar os campos no início de `POWER_MANAGEMENT_task()` apenas quando `esp_reset_reason() != ESP_RST_POWERON` e `pcm_rtc_diag_validate(&s_rtc_diag, target_freq, target_volt, dvfs_profile)` retornar `true`. Atualizar e selar `s_rtc_diag` sempre que `watchdog_resets`, `cml_error_count`, `last_watchdog_reason` ou os limites aprendidos mudarem, e antes de chamar `esp_restart()` no Nível 3.
- **Alternativa considerada:** Gravar contadores e limites aprendidos na NVS. Rejeitada devido ao limite de ciclos de escrita da Flash SPI em cenários de falha recorrente ou ciclos frequentes de ajuste DVFS.

### Decision 2: Rastreamento da causa do último Watchdog (`lastWatchdogReason`)
- **Escolha:** Adicionar `uint8_t watchdog_trigger_reason;` e `uint8_t last_watchdog_reason;` em `PowerManagementModule` (`main/tasks/power_management_task.h`). `hashrate_monitor_task.c` registra `1` para hashrate total `0.0 GH/s` por `60 s` e `2` para domínio congelado `< 10%` por `45 s` ao levantar `asic_watchdog_trigger`. `handle_asic_watchdog()` promove `last_watchdog_reason` para `watchdog_trigger_reason` (ou `3` antes de `esp_restart()` no Nível 3) e sela em `s_rtc_diag`.

### Decision 3: Campos de API e atualização do `SwarmComponent` (`AxeOS`)
- **Escolha:**
  - Adicionar ao `GET /api/system/info` (`main/http_server/system_api_json.c`) e `openapi.yaml`: `learnedMaxFreq`, `learnedMinVcore`, `lastWatchdogReason`, regenerando `src/app/generated/` (`npm run generate:api`) e atualizando o mock em `system.service.ts` conforme `AGENTS.md`.
  - Em `SwarmComponent` (`swarm.component.ts` / `swarm.component.html`):
    - Calcular `efficiency` por dispositivo (`axe.hashRate > 0 ? (axe.power / (axe.hashRate / 1000)) : 0`) e média da frota em `totals.efficiency` (`totals.hashRate > 0 ? (totals.power / (totals.hashRate / 1000)) : 0`).
    - Exibir card **Fleet Efficiency (`J/TH`)** no topo, coluna/badge de eficiência (`J/TH`), perfil DVFS (`Manual`, `Efficiency`, `Balanced`, `Performance`), clock efetivo + `VCore` (`+vdroopOffsetMv`) e tooltip de limite aprendido quando `> 0`.
    - Em `getDeviceNotification(axe)`: manter prioridade de alertas críticos (`miningPaused`, `overheat_mode === 1`, `power_fault`), usar `(axe.targetFrequency ?? axe.frequency) < 400` para `Frequency Low`, e incluir alertas para `axe.throttleActive === 1` (`Thermal Throttling`) e `axe.watchdogResets > 0` (`Watchdog Resets`).

## Risks / Trade-offs

- **Risco:** Mudança de layout de memória em atualização OTA de firmware invalidar o significado dos bytes em `RTC_NOINIT_ATTR`. → **Mitigação:** A validação exige correspondência simultânea de `magic` (e versão embutida ou tamanho/alinhamento), fingerprint dos alvos NVS e `CRC32`; qualquer divergência descarta o bloco com segurança como um *cold boot*.
- **Risco:** Placas legadas no Swarm (rodando firmware sem os novos campos `dvfsProfile`, `throttleActive`, `vdroopOffsetMv`) retornarem `undefined` para esses atributos. → **Mitigação:** Todas as exibições e cálculos no `SwarmComponent` tratam campos ausentes como opcionais (`?? 0` / `*ngIf`), mantendo compatibilidade total com dispositivos heterogêneos na rede.
