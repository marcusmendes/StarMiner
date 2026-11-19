# Design

## Context

Com a separação de tarefas entre o Core 0 (rede, HTTP, energia, térmica) e o Core 1 (despacho de jobs, leitura de nonces e monitor de hashrate) implementada na Fase 1, permaneceram quatro pontos de fragilidade herdados do `ESP-Miner` original — veja `proposal.md` para a motivação completa:
- Leitura de tacômetro sem guarda para `reading == 0` em `main/thermal/EMC2101.c`.
- Desligamento do ASIC em `mining_stop()` (`main/tasks/power_management_task.c`) sinalizando `ASIC_initalized = false` apenas ao final da rotina.
- Desreferenciamento do ponteiro compartilhado `active_job` / `active_jobs[job_id]` após a liberação de `valid_jobs_lock` em `main/tasks/asic_result_task.c` e `main/system.c`.
- Leitura não sincronizada de `HASHRATE_MONITOR_MODULE` em `main/http_server/system_api_json.c`.

## Goals / Non-Goals

**Goals:**
- Eliminar o risco de divisão inteira por zero (`ESP_RST_PANIC`) na leitura de RPM do `EMC2101`.
- Garantir que todas as tarefas do Core 1 interrompam novas transações na `UART1` antes de qualquer desaceleração de PLL ou corte de `VCore` durante `mining_stop()`.
- Tornar `ASIC_result_task` e `SYSTEM_notify_found_nonce` imunes a reciclagem concorrente dos slots de trabalho (`active_jobs`), mesmo quando o envio de shares via socket TCP sofrer atrasos.
- Serializar o acesso às medições de hashrate por domínio durante a construção do JSON de `/api/system/info` e broadcasts WebSocket.

**Non-Goals:**
- Alterar o protocolo de comunicação com o `BM1370`, a capacidade do anel de 16 jobs ou a política de reconexão Stratum.
- Modificar assinaturas externas da API HTTP ou chaves da NVS.

## Decisions

### Decision 1: Snapshot completo por valor de `bm_job` sob `valid_jobs_lock`
- **Escolha:** Em `ASIC_result_task` (`main/tasks/asic_result_task.c`), adquirir `valid_jobs_lock`, validar `valid_jobs[job_id]` e `active_jobs[job_id]`, copiar `*active_job` para uma struct `bm_job job_snapshot` na stack e copiar `active_job->jobid` e `active_job->extranonce2` para buffers locais dimensionados na stack (`jobid_buf[64]` e `extranonce2_buf[65]`) antes de chamar `pthread_mutex_unlock`. Em seguida, apontar `job_snapshot.jobid` e `job_snapshot.extranonce2` para esses buffers locais. Atualizar `SYSTEM_notify_found_nonce` (`main/system.h` / `main/system.c`) para receber `uint32_t job_target` (ou calcular `networkDifficulty(job_snapshot.target)`) em vez de acessar `active_jobs[job_id]` sem mutex.
- **Alternativa considerada:** Manter `valid_jobs_lock` travado durante toda a submissão de share via TCP/SV2. Rejeitada porque o socket pode bloquear por segundos em instabilidade de rede, travando `create_jobs_task` no Core 1.
- **Alternativa considerada:** Contagem de referência atômica em cada `bm_job`. Rejeitada por adicionar complexidade de gerenciamento de memória dinâmica em PSRAM sem necessidade, dado que `bm_job` e suas duas strings têm tamanho pequeno e previsível.

### Decision 2: Silenciamento prévio de `ASIC_initalized` em `mining_stop()`
- **Escolha:** Mover `GLOBAL_STATE->ASIC_initalized = false;` para a primeira ação executável de `mining_stop()` (`main/tasks/power_management_task.c`), seguida de um pequeno atraso de drenagem (`200 ms`) para que qualquer operação já iniciada por `create_jobs_task` ou `hashrate_monitor_task` na `UART1` termine antes da rampa de frequência para `50 MHz`, do corte de `VCore` (`0.0f`) e da ativação de `asic_hold_reset_low()`. Manter a espera final e `uart_flush(UART_NUM_1)`.
- **Alternativa considerada:** Adicionar um mutex exclusivo para `UART1` compartilhado entre Core 0 e Core 1. Rejeitada para evitar contenção e inversão de prioridade no caminho de latência mínima do Core 1; `mining_stop()` só ocorre em eventos raros de pausa, falha ou *watchdog*.

### Decision 3: Guarda explícita contra `0x0000` e `0xFFFF` em `EMC2101_get_fan_speed()`
- **Escolha:** Logo após montar `reading = tach_lsb | (tach_msb << 8)` em `main/thermal/EMC2101.c`, verificar `if (reading == 0 || reading == 0xFFFF) { return 0; }` antes de efetuar `5400000 / reading`, preservando o filtro existente para `RPM == 82` (equivalente a contagem saturada `0xFFFF`).
- **Alternativa considerada:** Tratar apenas `reading == 0`. Rejeitada porque `0xFFFF` representa tacômetro parado/desconectado no datasheet do `EMC2101` (`RPM == 82`) e retorná-lo explicitamente antes da divisão deixa a intenção clara.

### Decision 4: Sincronização de `HASHRATE_MONITOR_MODULE.lock` na serialização JSON
- **Escolha:** Em `system_api_add_hashrate_monitor()` (`main/http_server/system_api_json.c`), adquirir `pthread_mutex_lock(&g->HASHRATE_MONITOR_MODULE.lock)` antes de ler `total_measurement`, `error_measurement` e `domain_measurements` e liberar o mutex ao final do laço de leitura.
- **Alternativa considerada:** Copiar as medições para um snapshot local primeiro e liberar o mutex antes de criar nós `cJSON`. Como as alocações `cJSON` usam PSRAM e são rápidas (~4 domínios para 1 ASIC na Gamma), manter o lock apenas sobre a iteração de `asic_count` é simples e seguro desde que `HASHRATE_MONITOR_MODULE.is_initialized` garanta que `lock` foi inicializado (o que já ocorre em `hashrate_monitor_task`).

## Risks / Trade-offs

- **Risco:** `mining_stop()` adiciona `~200 ms` ao tempo de parada antes de cortar `VCore`. → **Mitigação:** Em cenários de emergência térmica (`>= 75 °C` ASIC ou `>= 105 °C` VR), a tarefa já estava em *Soft-Throttling* previamente; `200 ms` é desprezível frente à constante de tempo térmica do dissipador e evita corromper a `UART1` durante o reset.
- **Risco:** Strings `jobid` ou `extranonce2` maiores que os buffers locais na stack de `ASIC_result_task`. → **Mitigação:** `MAX_EXTRANONCE2_STR` já é `65` bytes em `create_jobs_task.c`; usar `snprintf`/`strncpy` seguro com terminação nula garantida e dimensionar `jobid_buf` com margem segura (`128` bytes, verificando uso de stack dentro dos `8192 B` da task).
