# Tasks: Calibração v2 do DVFS 2D Bandit (`Adaptive`) e Paridade do Coletor CSV

## 1. Calibração v2 do algoritmo `dvfs_bandit` e testes Unity

- [x] 1.1 Atualizar `components/asic/include/dvfs_bandit.h` e `components/asic/dvfs_bandit.c` para incluir `expected_hashrate_ghs` em `DvfsBanditSample` (com salvaguarda de estagnação quando `hashrate_ghs < 0.70f * expected_hashrate_ghs`), penalidade linear contínua de potência abaixo de `28.0 W`, prior físico sem viés de laço para vizinhos não visitados (e bloqueio de `df == 0, dv > 0` com `err_pct <= 0.3%`), além de `visit_count` e ajuste dos parâmetros de decaimento (`DISCOUNT_GAMMA = 0.995f`, `DRIFT_DECAY_GAMMA = 0.85f`, `DRIFT_ERR_THRESH_PCT = 1.5f`), verificando compilação limpa do componente `asic`.
- [x] 1.2 Expandir `components/asic/test/test_dvfs_bandit.c` com casos Unity para convergência ascendente a partir de frequência intermediária sem viés para `500 MHz`, penalidade contínua de potência em `VCore`, preservação de `visit_count` durante oscilações de erro `~0.6–1.0%` e fallback para hashrate medido em queda severa, validando 100% de aprovação via `./tools/run_qemu_tests.sh`.

## 2. Integração no firmware (`power_management_task`)

- [x] 2.1 Atualizar a montagem de `DvfsBanditSample` em `main/tasks/power_management_task.c` para passar `.expected_hashrate_ghs = power_management->expected_hashrate` e verificar compilação completa do firmware com `idf.py build` (com ambiente ESP-IDF v5.5.1 carregado).

## 3. Atualização do coletor de telemetria CSV

- [x] 3.1 Atualizar `scripts/collect_bitaxe_telemetry.py` adicionando `pre_watchdog_recoveries`, `adaptive_best_freq` e `adaptive_best_vcore` em `CSV_COLUMNS` e `build_csv_row()`, rastreamento de incrementos em `HostPrevState` / `emit_transition_alerts()`, exibição no resumo `--analyze` e asserções no `--self-test`.
- [x] 3.2 Validar `scripts/collect_bitaxe_telemetry.py` executando `python3 -m py_compile scripts/collect_bitaxe_telemetry.py`, `python3 scripts/collect_bitaxe_telemetry.py --self-test` e `python3 scripts/collect_bitaxe_telemetry.py --analyze backups/telemetry/20261125-222915/telemetry.csv`.

## 4. Verificação integrada e novo ciclo A/B de 24–48 h

- [x] 4.1 Executar a verificação integrada final antes do deploy (`./tools/run_qemu_tests.sh`, `idf.py build` e `python3 scripts/collect_bitaxe_telemetry.py --self-test`), confirmando zero falhas ou regressões nos perfis `0..3` e na Fase 1 (`domain-cusum-pre-watchdog`).
- [ ] 4.2 Realizar flash OTA nas placas e executar o ciclo A/B de 24–48 h com o coletor atualizado (`192.168.1.102` controle em `Balanced`, `192.168.1.103` piloto em `Adaptive`, e `192.168.1.101` em `Performance`/`Adaptive`), verificando convergência acima de `625–675 MHz`, estabilidade de `errorPercentage`, `pre_watchdog_recoveries` e eficiência `J/TH`.

## Workflow follow-up

- Após concluir a implementação e validar os ganhos no ciclo A/B de 24–48 h, arquivar esta change via `/openspec-archive-change` (mesclando os deltas de `vdroop-and-dvfs-autotuner` e `automated-test-coverage` para `openspec/specs/`).
