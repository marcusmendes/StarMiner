# Tasks

## 1. Redução de Tráfego Serial e Cache no Pipeline Stratum V1

- [x] 1.1 Em `components/stratum/stratum_api.c`, rebaixar `ESP_LOGI(TAG, "rx: %s", stratum_json)` para `ESP_LOGD` em `STRATUM_V1_parse()`, verificando com a suíte Stratum (`components/stratum/test/test_stratum_json.c` via `./tools/run_qemu_tests.sh`) que todas as mensagens Stratum V1 continuam sendo decodificadas sem regressões.
- [x] 1.2 Em `main/tasks/stratum_v1_task.c`, rebaixar os logs repetitivos intra-bloco de `decode_mining_notification()` (`BIP-54`, `BIP-110`, `Scriptsig`, `Coinbase outputs` e `Output %d`) de `ESP_LOGI` para `ESP_LOGD`, preservando `ESP_LOGI` apenas para trocas de `block_height` e mantendo intactas todas as atualizações de `GLOBAL_STATE`; verificar via build e inspeção de execução que o OLED e `/api/system/info` mantêm `scriptsig` e dados de coinbase corretos sem rajada `INFO` em `mining.notify`.

## 2. Desacoplamento de `s_log_mutex` da E/S Síncrona em Console

- [x] 2.1 Em `main/log_buffer.c`, refatorar `log_buffer_vprintf()` para preparar `stdout_buf` independente (`malloc(needed + 1)` quando `output == s_vprintf_buffer`, ou reutilização direta quando `output != s_vprintf_buffer`), liberar `s_log_mutex` com `xSemaphoreGiveRecursive(s_log_mutex)` antes de `fputs(stdout_buf, stdout)`, liberar `stdout_buf` após a escrita e manter fallback seguro sob `s_log_mutex` em caso de `malloc == NULL`; validar por teste unitário em `test/` / `./tools/run_qemu_tests.sh` cobrindo linha curta, linha longa (`>= 2048 B`) e integridade do ring buffer.

## 3. Isolamento do Barramento `I2C0`, IRAM-safe e Observabilidade de Retries

- [x] 3.1 Adicionar `CONFIG_I2C_ISR_IRAM_SAFE=y` em `sdkconfig.defaults` e sincronizar `sdkconfig` via `idf.py reconfigure`, verificando no mapa/resumo do `idf.py build` que o consumo de IRAM cabe com folga e a SRAM interna livre em operação atende à meta do projeto (`>= 110 KB`).
- [x] 3.2 Em `main/i2c_bitaxe.c`, extrair helper compartilhado de busca em `i2c_device_map` e emitir `ESP_LOGW` identificando `device_tag`, endereço I2C, tentativa e `esp_err_to_name(err)` antes de `i2c_master_bus_reset()` em `i2c_transfer_with_retries()`, verificando compilação sem warnings e ausência de duplicação na busca de dispositivos.
- [x] 3.3 Em `main/display.c`, definir `lvgl_cfg.task_affinity = 0;` antes de `lvgl_port_init(&lvgl_cfg)` em `display_init()`, confirmando em execução/monitoramento de tasks que `taskLVGL` permanece fixada no Core 0 junto às demais tasks de sistema e `I2C0`.

## 4. Verificação Integrada de Build, Testes e Hardware

- [x] 4.1 Executar `idf.py build` na raiz do repositório e rodar `./tools/run_qemu_tests.sh` (e build em `test-ci/`), confirmando 0 erros de compilação e 100% dos testes unitários aprovados.
- [x] 4.2 Realizar backup prévio e validação pós-flash na Bitaxe Gamma (`601a-4`) com `./scripts/backup_and_verify_gamma_v2.sh` (`backup-http`, `backup-flash`, `verify-post-flash`) e coletar logs contínuos por pelo menos 30 minutos (~50+ eventos `mining.notify`), confirmando eliminação dos avisos `TPS546 non-fatal STATUS_CML=0x02` correlacionados a `mining.notify` e estabilização de `cmlErrorCount`.

## Workflow follow-up

- Após implementação e validação em hardware, arquivar a mudança com `/openspec-archive-change` e atualizar os specs principais (`i2c-and-tps546-resilience`, `memory-and-core-pinning`, `stratum-and-console-log-resilience`).
