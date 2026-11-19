# Spec Delta

## ADDED Requirements

### Requirement: Persistência Pós-Reboot de Diagnóstico e Aprendizado DVFS em RTC_NOINIT
O sistema SHALL armazenar `watchdog_resets`, `cml_error_count`, `last_watchdog_reason` (`0..3`), `learned_max_freq_mhz` e `learned_min_vcore_mv` em memória `RTC_NOINIT_ATTR` protegida por `magic`, *fingerprint* dos alvos NVS e `CRC32`, restaurando-os após reinícios quentes (`esp_restart()`) sem gravação em Flash e expondo `learnedMaxFreq`, `learnedMinVcore` e `lastWatchdogReason` em `GET /api/system/info`.

#### Scenario: Reinício quente por software preserva contadores e limites DVFS aprendidos
- **WHEN** o dispositivo reinicia via `esp_restart()` (`esp_reset_reason() != ESP_RST_POWERON`) mantendo íntegros `magic`, `CRC32` e os valores configurados na NVS (`target_frequency`, `target_voltage`, `dvfs_profile`)
- **THEN** o sistema restaura `watchdog_resets`, `cml_error_count`, `last_watchdog_reason`, `learned_max_freq_mhz` e `learned_min_vcore_mv` da RAM RTC e os expõe em `/api/system/info`

#### Scenario: Cold boot ou alteração de alvo NVS descarta estado RTC anterior
- **WHEN** o dispositivo inicia por *power-on* (`ESP_RST_POWERON`), apresenta `CRC32` inválido ou detecta mudança nos alvos da NVS frente ao *fingerprint* armazenado
- **THEN** o sistema inicializa os contadores e limites aprendidos zerados com segurança sem corromper o estado operacional

### Requirement: Visão Comparativa de Frota (J/TH, DVFS e Alertas) na Página Swarm do AxeOS
A página `SwarmComponent` do AxeOS SHALL calcular e exibir a eficiência (`J/TH`) média da frota e individual por dispositivo, o perfil DVFS ativo (`0..3`), a frequência e tensão efetivas (com indicador de *Vdroop*), e alertas visuais para `throttleActive === 1` e `watchdogResets > 0`, avaliando frequência baixa por `(targetFrequency ?? frequency) < 400`.

#### Scenario: Frota de mineradores exibe eficiência J/TH, perfil DVFS e Vdroop lado a lado
- **WHEN** o usuário abre a página Swarm com dispositivos reportando `hashRate`, `power`, `dvfsProfile`, `frequency`, `coreVoltage` e `vdroopOffsetMv`
- **THEN** o painel exibe a eficiência agregada da frota (`J/TH`), o `J/TH` individual de cada placa, o badge do perfil DVFS e os valores efetivos de clock/tensão com tolerância a dispositivos legados sem esses campos

#### Scenario: Notificações do Swarm refletem throttling térmico e resets de watchdog sem falso positivo de frequência
- **WHEN** um dispositivo no Swarm reporta `throttleActive === 1` ou `watchdogResets > 0`, ou reporta `frequency < 400` enquanto `targetFrequency >= 400`
- **THEN** `getDeviceNotification()` exibe o alerta correspondente de *Thermal Throttling* ou *Watchdog* e não emite falso alerta de *Frequency Low* devido à redução transitória em RAM
