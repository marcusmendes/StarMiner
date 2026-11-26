# Proposal: Calibração v2 do DVFS 2D Bandit (`Adaptive`) e Paridade do Coletor CSV

## Why

A coleta pós-deploy em campo ([20261125-222915](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/backups/telemetry/20261125-222915/telemetry.csv)) revelou que, embora a Fase 1 (`domain-cusum-pre-watchdog`) tenha operado com zero falsos positivos, zero resets e zero novos erros CML, o perfil `4` (`Adaptive`) em `192.168.1.101` (`bitaxe-fdd0`) caminhou até o piso de `500 MHz` (`H_eff = 1048,0 GH/s`, `20,59 J/TH`) varrendo a tensão até `1250 mV`, enquanto a mesma placa em `dvfsProfile = 3` (`Performance`) sustentou `675,0 MHz @ 1175 mV` (`H_eff = 1372,7 GH/s`, `0,146%` de erro, `18,70 J/TH`). É necessário recalibrar o controlador 2D Bandit e alinhar as colunas do coletor CSV antes de iniciar o próximo ciclo A/B de 24–48 h.

## What Changes

- **Prior físico e desempate sem viés no UCB**: substituir a inicialização idêntica de vizinhos não visitados por um prior físico baseado no ganho esperado de frequência e custo energético do passo de tensão, eliminando o viés da ordem do laço (`df = -1, dv = -1` vencendo empates) e evitando elevação gratuita de `VCore` quando o erro atual já está baixo.
- **Sinal de $H_{\text{eff}}$ de baixa variância e penalidade contínua de potência**: alimentar o Bandit com `expected_hashrate_ghs * (1 - error_percentage / 100)` como base estável (com fallback para o hashrate medido se o chip estagnar) e aplicar penalidade linear contínua de potência abaixo de `28,0 W` (em adição à barreira íngreme acima de `28,0 W`), evitando decisões guiadas por ruído Poisson de `hashrate_1m` ou inflação de `VCore` em frequência fixa.
- **Memória persistente de células visitadas e decaimento calibrado**: rastrear explicitamente a contagem de visitas (`visit_count` / piso de `weight_n`) para que o decaimento por oscilações normais de erro não faça células já testadas voltarem a parecer nunca visitadas, além de calibrar limiares de decaimento temporal/por drift.
- **Atualização do coletor de telemetria CSV (`scripts/collect_bitaxe_telemetry.py`)**: exportar `pre_watchdog_recoveries`, `adaptive_best_freq` e `adaptive_best_vcore` em `CSV_COLUMNS` e `build_csv_row()`, emitir alertas em incrementos de `pre_watchdog_recoveries`, reportar essas métricas no resumo `--analyze` e cobri-las em `--self-test`.

## Capabilities

### New Capabilities

_(Nenhuma — os ajustes evoluem o comportamento existente do perfil `Adaptive` e sua cobertura de testes.)_

### Modified Capabilities

- `vdroop-and-dvfs-autotuner`: recalibração do requisito `Adaptive 2D Bandit DVFS Profile` com prior físico sem viés descendente, sinal $H_{\text{eff}}$ baseado em `expected_hashrate` com proteção contra estagnação, penalidade contínua de potência/eficiência e preservação da memória de células visitadas frente a pequenas oscilações de erro.
- `automated-test-coverage`: ampliação do requisito `Adaptive 2D Bandit Unit Tests in C` para cobrir convergência ascendente sem viés para `500 MHz`, penalidade contínua de potência por `VCore`, imunidade de visitação sob variações moderadas de erro e fallback de estagnação de `hashrate_1m`.

## Impact

- **Algoritmo 2D Bandit e testes C**: `components/asic/include/dvfs_bandit.h`, `components/asic/dvfs_bandit.c` e `components/asic/test/test_dvfs_bandit.c`.
- **Integração no firmware**: `main/tasks/power_management_task.c` (envio de `power_management->expected_hashrate` em `DvfsBanditSample`).
- **Coleta e análise A/B**: `scripts/collect_bitaxe_telemetry.py` (`CSV_COLUMNS`, `build_csv_row`, `HostPrevState`, `emit_transition_alerts`, `--analyze`, `--self-test`).
- **Sem quebra de contrato REST/OpenAPI/AxeOS**: os campos `dvfsProfile = 4`, `adaptiveBestFreq`, `adaptiveBestVcore` e `preWatchdogRecoveries` já existem no `openapi.yaml` e no JSON do firmware.
