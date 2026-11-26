# Tasks

## 1. Aritmética Pura EWMA+CUSUM e Testes Unitários (`power_control_math`)

- [x] 1.1 Se houver CSV coletado em `backups/telemetry/*/telemetry.csv`, executar `python3 scripts/collect_bitaxe_telemetry.py --analyze <arquivo>` para validar os percentis empíricos de `dom_min_ratio` e confirmar ou ajustar os parâmetros iniciais (`k = 0.78`, `h = 2.0`, `alpha = 0.20`); caso contrário, adotar os defaults documentados no `design.md` para calibração posterior.
- [x] 1.2 Em `components/asic/include/power_control_math.h` e `components/asic/power_control_math.c`, implementar `PcmDomainCusumState`, `pcm_domain_cusum_reset()`, `pcm_domain_cusum_update()` e `pcm_pre_watchdog_apply_relief()` respeitando os perfis DVFS `0..3` e os *Hard Caps* (`1000–1250 mV`, `400–675 MHz`, `<= 32.0 W`); verificar compilação limpa no componente `asic`.
- [x] 1.3 Em `components/asic/test/test_power_control.c`, adicionar casos `TEST_CASE` cobrindo rejeição de ruído instantâneo (`~0.78`), detecção de queda sustentada (`~0.50`), reset de estado em `pcm_domain_cusum_reset()` e regras de `pcm_pre_watchdog_apply_relief()` nos perfis `0..3` com e sem margem elétrica/térmica; validar aprovação de toda a suíte via `./tools/run_qemu_tests.sh`.

## 2. Monitoramento de Domínios a 1 Hz no Core 1 (`hashrate_monitor_task`)

- [x] 2.1 Em `main/tasks/hashrate_monitor_task.c`, alocar o estado `PcmDomainCusumState` por ASIC (`asic_count`, utilizando `MALLOC_CAP_SPIRAM` com fallback padrão), copiar os valores de `domain_measurements` para buffer local sob `HASHRATE_MONITOR_MODULE->lock` e executar `pcm_domain_cusum_update()` fora do mutex a `1 Hz` somente quando `watchdog_armed == true` e `hash_domains > 1`.
- [x] 2.2 Garantir reinício completo de `PcmDomainCusumState` sempre que `!watchdog_armed`, quando `pre_watchdog_trigger` já estiver pendente ou imediatamente após disparar `pre_watchdog_trigger = true` (registrando `pre_watchdog_asic`, `pre_watchdog_domain`, `pre_watchdog_cusum_max` e emitindo `ESP_LOGW`), confirmando preservação intacta do watchdog de 45 s / 60 s e compilação limpa sem warnings.

## 3. Recuperação Preventiva a Quente no Core 0 (`power_management_task`)

- [x] 3.1 Em `main/tasks/power_management_task.h`, adicionar os campos de contrato `pre_watchdog_trigger`, `pre_watchdog_asic`, `pre_watchdog_domain`, `pre_watchdog_cusum_max`, `pre_watchdog_recoveries` e `last_pre_watchdog_action_us` em `PowerManagementModule`, inicializando e limpando `pre_watchdog_trigger` em estados de pausa, falha, *overheat* e troca de alvo NVS em `main/tasks/power_management_task.c`.
- [x] 3.2 Em `main/tasks/power_management_task.c`, implementar `handle_pre_watchdog()` antes de `apply_effective_power_and_freq()` aplicando o *cooldown* de `60 s`, invocando `pcm_pre_watchdog_apply_relief()` sem chamar `mining_stop()`, atualizando `last_dvfs_step_us` e chamando `sync_rtc_diagnostics()` quando `learned_max_freq_mhz` ou `learned_min_vcore_mv` forem alterados; verificar via `idf.py build` que o firmware compila sem erros ou warnings.

## 4. Verificação Integrada de Build, QEMU e Hardware

- [x] 4.1 Executar build completo na raiz (`idf.py build`) e suíte completa em QEMU (`./tools/run_qemu_tests.sh` e build de `test/`), confirmando `0` falhas nos testes unitários e nenhum impacto em `openapi.yaml`, `partitions.csv` ou `freeHeapInternal`.
- [x] 4.2 Gravar o firmware na unidade piloto (`192.168.1.101`, usando os backups já existentes no projeto) e validar com `./scripts/backup_and_verify_gamma_v2.sh verify-post-flash 192.168.1.101` (`freeHeapInternal >= 110000`, `0` `power_fault`); replicar o rollout para `192.168.1.102` e `192.168.1.103` e validar via `python3 scripts/collect_bitaxe_telemetry.py` confirmando `0` falsos positivos e `0` panics.

## Workflow follow-up

- Após implementação e validação em hardware, arquivar a mudança com `/openspec-archive-change` (atualizando `openspec/specs/asic-hashrate-watchdog/spec.md` e `openspec/specs/automated-test-coverage/spec.md`) e iniciar `/openspec-propose` da Fase 2 (`adaptive-2d-dvfs-bandit`, `dvfsProfile = 4`) usando o baseline de 24h coletado.
