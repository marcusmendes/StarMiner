# Spec Delta

## ADDED Requirements

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
