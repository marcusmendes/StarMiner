# Design: Calibração v2 do DVFS 2D Bandit (`Adaptive`) e Paridade do Coletor CSV

## Context

Veja [proposal.md](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/openspec/changes/dvfs-bandit-v2-calibration/proposal.md) e os logs de campo em [20261124-210839](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/backups/telemetry/20261124-210839/telemetry.csv) e [20261125-222915](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/backups/telemetry/20261125-222915/raw.jsonl).

A inspeção da trajetória em `192.168.1.101` (`bitaxe-fdd0`) revelou que a implementação v1 em [dvfs_bandit.c](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/components/asic/dvfs_bandit.c) e [power_management_task.c](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/main/tasks/power_management_task.c) descia sistematicamente para `500 MHz` (`1048 GH/s`, `20,59 J/TH`) e varria tensões até `1250 mV` (`23,4 W`), apesar de a placa operar de forma estável em `675 MHz @ 1175 mV` (`1372,7 GH/s`, `18,70 J/TH`) sob o perfil `3` (`Performance`).

## Goals / Non-Goals

**Goals:**
- Eliminar o viés estrutural descendente em empates do UCB e impedir subida ociosa de `VCore` quando o erro já está baixo.
- Tornar a função de recompensa imune à variância Poisson de 1 minuto de `hashrate_1m`, mantendo proteção imediata se o hashrate real desabar.
- Preservar o histórico de células visitadas contra decaimentos excessivos provocados por oscilações normais de exploração.
- Expor `pre_watchdog_recoveries`, `adaptive_best_freq` e `adaptive_best_vcore` diretamente no `telemetry.csv`, alertas e `--analyze` de `scripts/collect_bitaxe_telemetry.py`.

**Non-Goals:**
- Não alterar a assinatura de endpoints HTTP, campos do `openapi.yaml` ou a UI do AxeOS (todos já compatíveis).
- Não alterar os limiares da Fase 1 (`domain-cusum-pre-watchdog`, que apresentou `0` falsos positivos em `~23,5 h + 9 h` de coleta) nem os perfis `0..3`.

## Decisions

### 1. Prior físico informado e desempate sem viés no UCB (`dvfs_bandit.c`)
- **Escolha**:
  - Para vizinhos não visitados (`visit_count == 0` ou `weight_n == 0`), calcular um prior físico otimista relativo à célula atual em vez de atribuir `cur_mean` idêntico a todos:
    - Estimar o ganho proporcional de $H_{\text{eff}}$ pela razão de frequência `(cand_freq / cur_freq)` sobre o hashrate normalizado atual.
    - Descontar uma estimativa física da variação de potência ($\Delta P \propto \Delta f + k_v \cdot \Delta V$) multiplicada pela penalidade contínua de eficiência.
  - Em empates numéricos no laço de vizinhos (`|df| <= 1, |dv| <= 1`), desempatar pelo maior prior / maior frequência esperada e menor `VCore` (nunca pela ordem do laço que favorecia `df = -1, dv = -1`).
  - Bloquear passos de aumento puro de tensão (`df == 0, dv > 0`) quando a taxa de erro atual já estiver limpa (`err_pct <= 0.3f`).
- **Alternativas consideradas**:
  - Embaralhar a ordem de iteração dos vizinhos: mitigaria parte do viés descendente, mas ainda desperdiçaria ciclos descendo de `625 MHz` para `500 MHz` antes de testar `637,5 MHz`.

### 2. Sinal de $H_{\text{eff}}$ de baixa variância + penalidade contínua de potência (`dvfs_bandit.{h,c}`, `power_management_task.c`)
- **Escolha**:
  - Estender `DvfsBanditSample` com `float expected_hashrate_ghs` (preenchido a partir de `power_management->expected_hashrate` em `power_management_task.c`).
  - Usar como hashrate-base da recompensa:
    - Se `expected_hashrate_ghs > 0.0f` e `hashrate_ghs >= 0.70f * expected_hashrate_ghs`, usar `expected_hashrate_ghs` (eliminando ruído Poisson de `±150 GH/s` de `hashrate_1m` que mascarava passos de `12,5 MHz` ≈ `25,5 GH/s`).
    - Caso `hashrate_ghs < 0.70f * expected_hashrate_ghs` (chip estagnado, domínio congelado ou drop real), usar `fminf(expected_hashrate_ghs, hashrate_ghs)` como salvaguarda para penalizar imediatamente o ponto instável.
    - Para chamadas unitárias ou legadas onde `expected_hashrate_ghs <= 0.0f`, manter compatibilidade usando `hashrate_ghs`.
  - Adicionar penalidade linear contínua de potência em toda a faixa útil (ex.: `DVFS_BANDIT_EFF_POWER_SLOPE` por Watt acima de uma referência basal de `18,0 W`), somada à barreira íngreme existente acima de `28,0 W`. Calibrada de modo que `+12,5 MHz` (`~+25,5 GH/s` ≈ `+0,021` na recompensa normalizada contra `~+0,55 W` ≈ `-0,0044` a `slope = 0.008`) permaneça líquido positivo, enquanto `+10 mV` em frequência fixa (`+0 GH/s`, `~+0,35 W`) resulte líquido negativo quando não houver ganho de estabilidade.
- **Alternativas consideradas**:
  - Trocar apenas `hashrate_1m` por `hashrate_10m`: a janela de `10 min` responde devagar demais após cada passo de `60 s` do Bandit e ainda contamina células vizinhas com médias antigas após uma transição.

### 3. Memória explícita de visitação e calibragem de decaimento (`dvfs_bandit.{h,c}`)
- **Escolha**:
  - Incluir `uint16_t visit_count` em `DvfsBanditCell` para distinguir inequivocamente célula "nunca visitada" (`visit_count == 0`) de célula já visitada com peso antigo reduzido (`visit_count > 0`, mantendo piso mínimo em `weight_n` quando visitada).
  - Ajustar `DVFS_BANDIT_DISCOUNT_GAMMA` (`0.98f -> 0.995f`), `DVFS_BANDIT_DRIFT_DECAY_GAMMA` (`0.70f -> 0.85f`) e `DVFS_BANDIT_DRIFT_ERR_THRESH_PCT` (`0.6f -> 1.5f`) para que oscilações transitórias típicas de exploração (`0,6%–1,2%`) não zerem o mapa aprendido a cada poucos minutos, reservando o decaimento rápido a drift térmico real (`>= 2,5 °C`) ou salto severo de erro (`>= 1,5%`).
- **Alternativas consideradas**:
  - Desabilitar totalmente o decaimento por erro: retardaria a adaptação caso um lote de silício comece a degradar após mudança de fonte ou pasta térmica.

### 4. Paridade CSV no coletor Python (`scripts/collect_bitaxe_telemetry.py`)
- **Escolha**:
  - Adicionar `pre_watchdog_recoveries`, `adaptive_best_freq` e `adaptive_best_vcore` ao final de `CSV_COLUMNS` e preencher em `build_csv_row()` a partir de `preWatchdogRecoveries`, `adaptiveBestFreq` e `adaptiveBestVcore`.
  - Rastrear `pre_watchdog_recoveries` em `HostPrevState` e `emit_transition_alerts()` (emitindo alerta em tempo real quando o contador aumentar), incluir essas colunas no resumo `--analyze` (mostrando último ponto seguro adaptativo e total de recuperações CUSUM) e validá-las no `--self-test`.
- **Alternativas consideradas**:
  - Extrair apenas via `raw.jsonl` com scripts ad-hoc: dificulta inspeção rápida e regressão comparativa entre campanhas CSV.

## Risks / Trade-offs

- **[Risco] Prior físico exagerado fazer o Bandit tentar escalar acima do teto ótimo rápido demais em placa com margem térmica justa** → **Mitigação**: Manter passos máximos de `±1` (`12,5 MHz` / `10 mV`), histerese mínima de `2` ciclos (`120 s`), bloqueio `allow_positive` (`chip_temp < 64.5 °C`, `vr_temp < 82.0 °C`, `power < 30.0 W`) e fallback imediato pelo `pre_watchdog_trigger` da Fase 1.
- **[Risco] Limiar de fallback (`0.70 * expected_hashrate`) não pegar perda parcial de 1 único domínio (`~25%`) em casos limítrofes** → **Mitigação**: Um domínio inteiro parado ativa diretamente o `pre_watchdog_trigger` da Fase 1 (`CUSUM`), além de refletir em queda proporcional abaixo de `0.75 * expected_hashrate` e poder ser ajustado em conjunto se necessário; testes Unity cobrirão queda parcial severa (`< 0.70`).
- **[Risco] Alteração de `CSV_COLUMNS` quebrar scripts que leiam colunas por posição fixa** → **Mitigação**: Novas colunas adicionadas ao final da lista e todo consumidor interno lê por nome via `csv.DictReader`.

## Migration Plan

1. Aplicar calibragem v2 em `dvfs_bandit.{h,c}` + `power_management_task.c` + `test_dvfs_bandit.c` e executar `./tools/run_qemu_tests.sh` e `idf.py build`.
2. Atualizar `scripts/collect_bitaxe_telemetry.py` e validar com `python3 -m py_compile`, `--self-test` e `--analyze` sobre [backups/telemetry/20261125-222915/telemetry.csv](file:///Users/marcusmaciel/Projects/pessoal/StarMiner/backups/telemetry/20261125-222915/telemetry.csv).
3. Realizar flash OTA nas placas e conduzir o novo ciclo A/B de 24–48 h iniciando o coletor atualizado:
   - `192.168.1.102` (`bitaxe`) em `dvfsProfile = 2` (`Balanced`) como controle.
   - `192.168.1.103` (`bitaxe-41ac`, melhor margem térmica/potência da frota: `20,9 W`, fan `~36%`) como piloto `dvfsProfile = 4` (`Adaptive` v2).
   - `192.168.1.101` (`bitaxe-fdd0`) em `dvfsProfile = 3` (`Performance`) ou também em `Adaptive` (`4`) após validação inicial de `.103`, comparando `H_eff`, `J/TH`, `pre_watchdog_recoveries` e convergência `(adaptive_best_freq, adaptive_best_vcore)`.
4. **Rollback**: Caso qualquer unidade piloto regrida frente ao controle, alternar `dvfsProfile` para `2` ou `3` via AxeOS / `PATCH /api/system` sem necessidade de reflash.
