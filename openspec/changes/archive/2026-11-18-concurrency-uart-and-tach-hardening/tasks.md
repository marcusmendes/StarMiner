# Tasks

## 1. Blindagem da Leitura de Tacômetro (`EMC2101`)

- [x] 1.1 Adicionar guarda para `reading == 0 || reading == 0xFFFF` antes de `5400000 / reading` em `EMC2101_get_fan_speed()` (`main/thermal/EMC2101.c`), cobrindo o cálculo de RPM com teste unitário em `components/asic/test/` ou helper de conversão verificável via `./tools/run_qemu_tests.sh`.

## 2. Silenciamento Prévio da `UART1` em `mining_stop()`

- [x] 2.1 Mover `GLOBAL_STATE->ASIC_initalized = false` para o início de `mining_stop()` em `main/tasks/power_management_task.c` com atraso de drenagem (`200 ms`) antes de `ASIC_set_frequency()`, `ASIC_set_nonce_space()`, `VCORE_set_voltage(0.0f)` e `asic_hold_reset_low()`, verificando nos logs de execução em hardware ou teste que nenhuma escrita de job ocorre durante a rampa para `50 MHz`.

## 3. Snapshot Imutável de `bm_job` e Lock de `hashrateMonitor`

- [x] 3.1 Em `ASIC_result_task()` (`main/tasks/asic_result_task.c`), copiar por valor `bm_job` e buffers locais de `jobid` e `extranonce2` sob `valid_jobs_lock` antes de liberar o mutex, e atualizar `SYSTEM_notify_found_nonce()` (`main/system.h` e `main/system.c`) para receber `uint32_t job_target` em vez de indexar `active_jobs[job_id]` sem lock; verificar compilação sem warnings e ausência de leituras desprotegidas de `active_jobs`.
- [x] 3.2 Proteger a leitura de `total_measurement`, `error_measurement` e `domain_measurements` em `system_api_add_hashrate_monitor()` (`main/http_server/system_api_json.c`) com `pthread_mutex_lock(&g->HASHRATE_MONITOR_MODULE.lock)` e `pthread_mutex_unlock`, verificando que `/api/system/info` retorna `hashrateMonitor` consistente sem deadlock.

## 4. Verificação de Build e Suíte de Testes

- [x] 4.1 Executar `idf.py build` na raiz do projeto, `idf.py build` em `test-ci/` (e `./tools/run_qemu_tests.sh`) confirmando 0 erros de compilação e 100% dos testes passando.
