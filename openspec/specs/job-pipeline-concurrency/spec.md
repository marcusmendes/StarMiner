# job-pipeline-concurrency Specification

## Purpose

Assegura a integridade concorrente (*thread-safety*) do ciclo de vida das notificações Stratum (`mining_notify`), da fila de trabalho (`work_queue`) e dos jobs ativos do ASIC (`active_jobs` / `valid_jobs`) entre os núcleos Core 0 e Core 1.

## Requirements

### Requirement: Decodificação de `mining_notification` antes do enfileiramento no Stratum V1
O sistema SHALL concluir toda leitura e decodificação de `mining_notification` (`decode_mining_notification`) antes de publicar o ponteiro em `stratum_queue` via `queue_enqueue`.

#### Scenario: Consumo imediato de notificação pelo Core 1
- **WHEN** `stratum_v1_task` no Core 0 recebe um método `MINING_NOTIFY` e enfileira `mining_notification` para `create_jobs_task` no Core 1
- **THEN** `decode_mining_notification` já foi executado antes de `queue_enqueue`, impedindo qualquer leitura pós-liberação (*Use-After-Free*) caso `create_jobs_task` processe e libere a notificação imediatamente

### Requirement: Sincronização de `active_jobs` e `valid_jobs` sob `valid_jobs_lock` no driver BM1370
O sistema SHALL proteger a liberação do job anterior (`free_bm_job`), a substituição de `active_jobs[job_id]` e a atualização/leitura de `valid_jobs[job_id]` e `active_jobs[job_id]->version` dentro da região crítica do mutex `valid_jobs_lock` tanto em `BM1370_send_work` quanto em `BM1370_process_work`.

#### Scenario: Concorrência entre despacho de novo job e recepção de nonce do mesmo `job_id`
- **WHEN** `create_jobs_task` substitui `active_jobs[job_id]` simultaneamente à recepção de um resultado do `BM1370` por `ASIC_result_task` para o mesmo `job_id`
- **THEN** o acesso a `valid_jobs[job_id]` e `active_jobs[job_id]` é serializado por `valid_jobs_lock`, prevenindo desreferenciamento de ponteiro liberado ou nulo

### Requirement: Limpeza integral de `valid_jobs` sem zerar o monitor de hashrate em `clean_jobs`
O sistema SHALL zerar todos os 128 índices do vetor `valid_jobs` sob `valid_jobs_lock` durante `SYSTEM_clean_jobs_queue` e MUST NOT invocar `hashrate_monitor_reset_measurements` em eventos de `clean_jobs` do Stratum.

#### Scenario: Recepção de `mining.notify` com `clean_jobs = true` em troca de bloco
- **WHEN** a pool envia `clean_jobs = true` e `SYSTEM_clean_jobs_queue` é executado
- **THEN** todos os 128 índices de `valid_jobs` são invalidados atomicamente sob `valid_jobs_lock` e as medições contínuas de hashrate permanecem preservadas sem queda artificial para zero

### Requirement: Deadline absoluto único e validação de timeout em `queue_dequeue_timeout`
O sistema SHALL validar `timeout_ms <= 0` e calcular o instante absoluto `timeout_time` uma única vez antes do laço `while (queue->count == 0)` em `queue_dequeue_timeout`.

#### Scenario: Despertar espúrio antes do esgotamento do timeout na fila de trabalho
- **WHEN** `pthread_cond_timedwait` retorna sem item na fila devido a um despertar espúrio antes de `timeout_time`
- **THEN** o laço reutiliza o mesmo deadline absoluto `timeout_time` sem reiniciar a contagem do timeout e sem gerar `tv_nsec` inválido (`EINVAL`)

### Requirement: Snapshot imutável de job ativo durante processamento de nonce
Ao processar um nonce retornado pelo ASIC, o sistema SHALL copiar por valor os metadados do job correspondente (`target`, `version`, `pool_diff`, `ntime`, `jobid` e `extranonce2`) enquanto retém `valid_jobs_lock`, proibindo qualquer leitura posterior do ponteiro compartilhado após a liberação do mutex.

#### Scenario: Submissão de share via rede simultânea à reciclagem do slot no anel do ASIC
- **WHEN** um nonce válido é encontrado e o envio da submissão Stratum atrasa enquanto novos jobs sobrescrevem o mesmo índice no vetor de jobs ativos
- **THEN** o processamento pós-envio, o log do share, a notificação de dificuldade máxima e o registro no scoreboard utilizam exclusivamente o snapshot local imutável sem ler memória liberada

### Requirement: Silenciamento prévio do barramento UART antes do desligamento do ASIC
Durante a parada da mineração em `mining_stop`, o sistema SHALL invalidar o estado de inicialização do ASIC (`ASIC_initalized = false`) e aguardar a drenagem de transações em curso antes de desacelerar a frequência, cortar a tensão de núcleo (`VCore`) e acionar o pino de reset.

#### Scenario: Parada de mineração com despacho de jobs e leitura de registradores ativos no Core 1
- **WHEN** `mining_stop` é iniciado no Core 0 enquanto `create_jobs_task` e `hashrate_monitor_task` executam no Core 1
- **THEN** o sistema marca `ASIC_initalized = false` e aguarda a conclusão das operações em andamento no Core 1 antes de alterar o PLL para `50 MHz` ou zerar o `VCore`

### Requirement: Leitura sincronizada das medições de hashrate na serialização JSON
Ao montar o objeto `hashrateMonitor` para respostas HTTP ou broadcasts WebSocket, o sistema SHALL adquirir `HASHRATE_MONITOR_MODULE.lock` durante toda a leitura de `total_measurement`, `error_measurement` e `domain_measurements`.

#### Scenario: Consulta à API durante atualização concorrente dos registradores de domínio
- **WHEN** `system_api_add_hashrate_monitor` lê os contadores de domínio enquanto o monitor de hashrate no Core 1 atualiza os registradores do ASIC
- **THEN** a serialização lê um estado consistente sob `HASHRATE_MONITOR_MODULE.lock` e libera o mutex ao concluir a leitura
