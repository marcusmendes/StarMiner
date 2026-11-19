# Spec Delta

## ADDED Requirements

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
