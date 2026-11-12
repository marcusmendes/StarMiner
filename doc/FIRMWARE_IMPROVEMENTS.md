# Plano de Melhorias de Firmware — StarMiner Bitaxe Gamma (`GammaV2` / `601a-4`)

Documento de especificação técnica e memória do projeto para evoluir o firmware [`ustarminer/StarMiner`](https://github.com/ustarminer/StarMiner) (derivado do `bitaxeorg/ESP-Miner`) com foco na placa **StarMiner Bitaxe Gamma 601 (`GammaV2` / `601a-4`, DC 12 V, BM1370)**.

---

## 1. Baseline do Hardware e Diagnóstico Inicial (2026-11-10)

Leitura coletada via `GET /api/system/info` no dispositivo em operação (`192.168.1.108`):

| Parâmetro | Valor observado | Observação |
| :--- | :--- | :--- |
| Modelo / Board / ASIC | `LT-001` / `601a-4` / `BM1370` (`GammaV2`) | 2040 small cores, 4 domínios, entrada DC 12 V (`maxPower: 40 W`) |
| Firmware / AxeOS / IDF | `V1.0.1-rc.10` / `V1.0.1-rc.10` / `v5.5.2` | Partição em execução: `factory` |
| Frequência / VCore | `550 MHz` / `1150 mV` (`1138–1142 mV` reais) | Queda resistiva (*Vdroop*) de `8–12 mV` sob carga |
| Potência / Eficiência | `21,62 W` (`12,03 V` entrada) / `~19,4 J/TH` | `maxPower: 40 W` (`selfTestParams` testado até `600 MHz` / `22,34 W`) |
| Hashrate / Domínios | `1.114,7 GH/s` média (`1.164,8 GH/s` inst.) | Domínios: `[286,9 \| 303,2 \| 311,8 \| 270,6] GH/s` |
| Erros de Hardware | `0,2287%` média (`0,0737%` inst.) | Operação bastante estável em `550 MHz` |
| Térmica / Fan | `temp: 60 °C`, `vrTemp: 60 °C`, `45,8%` (`5.702 RPM`) | Alvo `temptarget: 60 °C`, ampla folga de resfriamento |
| Stratum / Shares | `12.139` aceitos / `6` rejeitados (`Stale`) | `responseTime: 208,8 ms` em `parasite.wtf:42069` |
| Memória Heap Livre | `7,91 MB` total (`Internal: 22,9 kB` / `Spiram: 7,95 MB`) | **SRAM interna criticamente baixa (`22,9 kB`)** |
| Falhas Registradas | `TPS546D24A STATUS_CML=0x02 COMM` | Erro de comunicação I2C/PMBus (`status_cml: 2`) |
| Reset Reason / Uptime | `Software reset via esp_restart` / `~13h45m` | — |

---

## 2. Alertas Críticos e Bugs Encontrados no Código-Base

### 2.1. Divergência entre o repositório público e a build de fábrica da placa
* A placa roda **`V1.0.1-rc.10`** (`model: "LT-001"`, `boardVersion: "601a-4"`, `ESP-IDF v5.5.2`), contendo recursos não presentes neste repositório (`master` em `3bf9c3e`), como `selfTestParams` e o histórico `error-code`.
* **Regra obrigatória:** nunca gravar firmware compilado aqui sem antes fazer **backup completo da flash (16 MB) e da partição NVS (`24 KB`)** via USB-C.

### 2.2. Bug de reconhecimento da placa em `main/device_config.h` e `main/device_config.c`
* [`main/device_config.h`](../main/device_config.h#L145) cadastra `"601a"` em `default_configs[]`, mas **não cadastra `"601a-4"`**.
* Quando [`device_config_init`](../main/device_config.c#L32-L46) não encontra `"601a-4"` em `default_configs[]`, ele cai no fluxo *Custom Board Version* e busca `devicemodel` no array [`default_families[]`](../main/device_config.h#L117-L125).
* **Duplo problema:**
  1. `FAMILY_GAMMA_V2` e `FAMILY_GAMMA_DUO` **não foram incluídas em `default_families[]`** (linhas 117–125).
  2. Na placa StarMiner, a NVS de fábrica traz `devicemodel = "LT-001"` (e não `"GammaV2"`). Portanto, mesmo com `FAMILY_GAMMA_V2` em `default_families[]`, qualquer revisão futura (`"601a-5"`, etc.) ainda falharia no fallback por família se não houver correspondência por prefixo `"601a*"` ou alias `"LT-001"`.
* Sem essa correção, `family` permanece zerada (`voltage_domains = 0`, `nominal_voltage = 0`), causando falha em [`VCORE_init`](../main/power/vcore.c#L104) ou queda no perfil padrão de `5 V` (`VIN_OV_FAULT = 6,5 V`) do `TPS546` em [`get_tps546_config`](../main/power/vcore.c#L81-L97).

### 2.3. Causa raiz do `STATUS_CML=0x02 COMM`: Tráfego I2C redundante no `TPS546` a cada 100 ms
* Em [`main/power/TPS546.c`](../main/power/TPS546.c#L290-L306), toda chamada a [`TPS546_get_vout`](../main/power/TPS546.c#L562) invoca [`ulinear16_2_float`](../main/power/TPS546.c#L290), que faz uma leitura I2C do registrador estático `PMBUS_VOUT_MODE` (`0x20`).
* Além disso, a cada **100 ms** ([`main/tasks/power_management_task.c`](../main/tasks/power_management_task.c#L139-L141)), a `POWER_MANAGEMENT_task` chama [`Power_get_output`](../main/power/power.c#L15) (que chama `TPS546_get_vout()`) e na linha seguinte chama [`VCORE_get_voltage_mv`](../main/power/vcore.c#L118) (que chama `TPS546_get_vout()` **novamente**).
* Isso gera **30 transações I2C por segundo** redundantes no barramento `I2C0`, concorrendo diretamente com o `EMC2101` (lido a cada 100 ms por duas tasks) e com a atualização do display OLED (`LVGL`).

### 2.4. Bugs de concorrência (Use-After-Free) e reset indevido no pipeline de Jobs
1. **Use-After-Free em [`main/tasks/stratum_v1_task.c`](../main/tasks/stratum_v1_task.c#L353-L354):**
   `stratum_v1_task` chama `queue_enqueue(&GLOBAL_STATE->stratum_queue, stratum_api_v1_message.mining_notification)` na linha 353 e, **após transferir a posse do ponteiro para a fila**, lê os campos de `mining_notification` em [`decode_mining_notification`](../main/tasks/stratum_v1_task.c#L60) na linha 354. Com *Core Pinning*, a `create_jobs_task` no Core 1 pode acordar imediatamente, processar e liberar `mining_notification` enquanto o Core 0 ainda executa `decode_mining_notification`.
2. **Race Condition (Use-After-Free) em [`components/asic/bm1370.c`](../components/asic/bm1370.c#L341-L396):**
   Em [`BM1370_send_work`](../components/asic/bm1370.c#L341-L347), `free_bm_job(active_jobs[job.job_id])` é chamado **antes** de adquirir o mutex `valid_jobs_lock`. Já em [`BM1370_process_work`](../components/asic/bm1370.c#L390-L395) (`ASIC_result_task`), `active_jobs[job_id]->version` é desreferenciado **sem adquirir `valid_jobs_lock`**.
3. **Reset indevido de hashrate em [`main/system.c`](../main/system.c#L273-L286):**
   [`SYSTEM_clean_jobs_queue`](../main/system.c#L284) chama `hashrate_monitor_reset_measurements(GLOBAL_STATE)` sempre que `stratum_queue.count > 0` em um `clean_jobs`, zerando as medições de hashrate do minerador em trocas de bloco.
4. **Cálculo de timeout na fila de jobs em [`main/work_queue.c`](../main/work_queue.c#L54-L79):**
   `queue_dequeue_timeout` recalcula `timeout_time` dentro do loop `while (queue->count == 0)` e não protege contra `timeout_ms <= 0`, o que pode gerar `tv_nsec` inválido (`EINVAL`) em `pthread_cond_timedwait`.

---

## 3. O que JÁ está implementado neste código (não reimplementar)

* **`TCP_NODELAY` no Stratum V1:** configurado em [`components/stratum/stratum_socket.c`](../components/stratum/stratum_socket.c#L163).
* **Stratum V2:** implementação completa em `components/stratum_v2/` (`sv2_noise.c`, `sv2_protocol.c`) e [`main/tasks/stratum_v2_task.c`](../main/tasks/stratum_v2_task.c).
* **Alocação PSRAM para `cJSON`, Wi-Fi/LwIP e mbedTLS:** hooks de `cJSON` em [`main/main.c`](../main/main.c#L62-L66) e diretivas em [`sdkconfig.defaults`](../sdkconfig.defaults#L12-L37).
* **Sinalização imediata da `stratum_queue`:** [`queue_enqueue`](../main/work_queue.c#L39) já dispara `pthread_cond_signal(&queue->not_empty)`, acordando imediatamente [`create_jobs_task`](../main/tasks/create_jobs_task.c#L84) bloqueada em `queue_dequeue_timeout` (não sendo necessário `xTaskNotifyGive`).

---

## 4. Especificação das Melhorias de Firmware

### Fase 0 — Base Segura, Compatibilidade `601a-4` / `LT-001` e Backup
1. **Backup físico antes de qualquer gravação:** dump integral da flash (16 MB) e da partição `nvs` (`0x9000`, `0x6000` em [`partitions.csv`](../partitions.csv#L3)) via cabo USB-C de dados (`esptool`).
2. **Registro completo em [`main/device_config.h`](../main/device_config.h#L117-L152):**
   * Adicionar `FAMILY_GAMMA_V2` e `FAMILY_GAMMA_DUO` em `default_families[]`.
   * Adicionar entrada explícita `"601a-4"` em `default_configs[]` (espelhando `"601a"` com `FAMILY_GAMMA_V2`, `TPS546 = true`, `EMC2101 = true`, `power_consumption_target = 22`).
3. **Fallback resiliente em [`main/device_config.c`](../main/device_config.c#L32-L46):**
   * Se não houver match exato de `board_version` em `default_configs[]`, verificar match por prefixo `"601a"` (`strncmp(board_version, "601a", 4) == 0`) apontando para a configuração da `601a`.
   * No fallback de `default_families[]`, tratar o alias `devicemodel = "LT-001"` como `FAMILY_GAMMA_V2`.
4. **Validação de build limpa:** confirmar ambiente **ESP-IDF (`v5.5.x`)** e validar compilação limpa antes das fases seguintes.

---

### Fase 1 — Liberação de ~90 KB de SRAM Interna, *Core Pinning* e Pipeline Thread-Safe

#### 4.1. Alívio de SRAM Interna (`22,9 kB` → alvo `> 110 kB` livres)
Todas as alocações em PSRAM devem verificar `esp_psram_is_initialized()` (com fallback para `MALLOC_CAP_DEFAULT`), conforme diretriz do [`AGENTS.md`](../AGENTS.md).

* **Stacks de tasks elegíveis para PSRAM (`xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)`):**
  * Em [`main/main.c`](../main/main.c#L138-L185):
    * `POWER_MANAGEMENT_task` (`8192 B`)
    * `FAN_CONTROLLER_task` (`8192 B`)
    * `create_jobs_task` (`8192 B`)
    * `ASIC_result_task` (`8192 B`)
    * `statistics_task` (`8192 B`)
    * `hashrate_monitor_task` (`4096 B`)
  * Em [`main/http_server/websocket_log.c`](../main/http_server/websocket_log.c#L168): `websocket_log_task` (`4096 B`).
  * Em [`main/bap/bap_uart.c`](../main/bap/bap_uart.c#L72-L89) e [`main/bap/bap_handlers.c`](../main/bap/bap_handlers.c#L54): `uart_receive_ta` (`4096 B`), `uart_send_task` (`4096 B`) e `bap_mode_mgmt` (`2048 B`).
  * **Subtotal liberado em stacks:** **~58 KB de SRAM interna**.
  * **Restrição obrigatória (manter em SRAM interna):**
    * `nvs_task` ([`main/nvs_config.c`](../main/nvs_config.c#L413-L414)), pois realiza escrita direta na NVS/Flash.
    * Task do servidor `httpd` ([`main/http_server/http_server.c`](../main/http_server/http_server.c#L1428)), pois os handlers `POST_OTA_update` e `POST_WWW_update` executam `esp_ota_write` / `esp_partition_erase_range` diretamente na Flash SPI.
* **Buffers de Heap migrados para PSRAM (`heap_caps_malloc` / `heap_caps_calloc` com `MALLOC_CAP_SPIRAM`):**
  1. **`rest_server_context_t` (`10,5 KB`):** em [`main/http_server/http_server.c`](../main/http_server/http_server.c#L1424), alocar `rest_context` (que contém `char scratch[10240]`) na PSRAM.
  2. **`scratch_buf` do WebSocket (`4 KB`):** em [`main/http_server/websocket_log.c`](../main/http_server/websocket_log.c#L22), alocar `WS_LOG_CHUNK_SIZE` na PSRAM.
  3. **Buffer RX do BAP (`1 KB`):** em [`main/bap/bap_uart.c`](../main/bap/bap_uart.c#L118), alocar `rx_buf` (`BAP_BUF_SIZE`) na PSRAM.
  4. **Pool/alocações de `bm_job` (`~10 KB`):** em [`main/tasks/create_jobs_task.c`](../main/tasks/create_jobs_task.c#L206-L400) (`generate_work`, `generate_work_sv2`, `generate_work_sv2_ext`), alocar structs `bm_job` e strings `jobid` / `extranonce2` na PSRAM.
  5. **Buffers JSON e `mining_notify` do Stratum (`~6 KB`):** em [`components/stratum/stratum_api.c`](../components/stratum/stratum_api.c#L98-L318), alocar `json_rpc_buffer` e as structs `mining_notify` (incluindo `job_id`, `prev_block_hash`, `coinbase_1`, `coinbase_2` e `merkle_branches`) na PSRAM.
  * **Subtotal liberado em buffers:** **~31,5 KB de SRAM interna** (Total Fase 1: **~89,5 KB liberados**).

#### 4.2. Isolamento de Núcleos (*Core Pinning*) e Correções no Pipeline de Jobs
* **Arquitetura de Core Pinning:**
  * **Core 1 (Dedicado ao Tempo Real de Mineração):**
    * `ASIC_result_task` (prioridade `15`) — leitura contínua da UART do `BM1370` e validação de nonces sem interferência de interrupções de Wi-Fi ou HTTP.
    * `create_jobs_task` (prioridade `10`) — montagem de cabeçalhos/merkle root e despacho imediato de trabalho para o ASIC.
    * `hashrate_monitor_task` (prioridade `5`) — amostragem determinística dos registradores de hashrate por domínio.
  * **Core 0 (Sistema, Rede, Protocolo Stratum e Sensores):**
    * Wi-Fi / LwIP (nativo no Core 0), servidor `httpd` (`config.core_id = 0` em [`http_server.c`](../main/http_server/http_server.c#L1428)), `websocket_log_task`.
    * `protocol_coordinator_task`, `stratum_v1_task` e `stratum_v2_task`.
    * `POWER_MANAGEMENT_task`, `FAN_CONTROLLER_task`, `statistics_task`, `nvs_task` e tasks do BAP.
* **Correções de concorrência no pipeline de Jobs:**
  1. Em [`main/tasks/stratum_v1_task.c`](../main/tasks/stratum_v1_task.c#L353-L354), executar `decode_mining_notification` **antes** de `queue_enqueue` para eliminar o Use-After-Free entre Core 0 e Core 1.
  2. Em [`components/asic/bm1370.c`](../components/asic/bm1370.c#L341-L396):
     * Em `BM1370_send_work`, mover `free_bm_job(active_jobs[job.job_id])` para **dentro** da região crítica protegida por `pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock)`.
     * Em `BM1370_process_work`, envolver a verificação de `valid_jobs[job_id]` e a leitura de `active_jobs[job_id]->version` com `pthread_mutex_lock(&GLOBAL_STATE->valid_jobs_lock)` / `pthread_mutex_unlock`.
  3. Em [`main/system.c`](../main/system.c#L273-L286) (`SYSTEM_clean_jobs_queue`), zerar todos os 128 índices de `valid_jobs` sob `valid_jobs_lock` e **remover** a chamada a `hashrate_monitor_reset_measurements(GLOBAL_STATE)`.
  4. Em [`main/work_queue.c`](../main/work_queue.c#L54-L79) (`queue_dequeue_timeout`), calcular o deadline absoluto `timeout_time` **antes** do `while (queue->count == 0)` e tratar `timeout_ms <= 0` de forma segura.

---

### Fase 2 — Otimização I2C (`TPS546D24A`) e Watchdog de 3 Níveis do `BM1370`

#### 4.3. Redução de Tráfego I2C, Tratamento de `STATUS_CML=0x02 COMM` e Bus Reset
* **Cache de `PMBUS_VOUT_MODE` e eliminação de leitura dupla (`main/power/TPS546.c` e `main/tasks/power_management_task.c`):**
  * Em [`TPS546.c`](../main/power/TPS546.c#L290-L306), ler `PMBUS_VOUT_MODE` (`0x20`) uma única vez durante `TPS546_init()` e armazenar em cache estático (`s_vout_mode`), eliminando transações I2C dentro de `ulinear16_2_float()` e `float_2_ulinear16()`.
  * Em [`TPS546.c`](../main/power/TPS546.c#L562) / [`power_management_task.c`](../main/tasks/power_management_task.c#L139-L141), reaproveitar a tensão lida em `Power_get_output()` para `VCORE_get_voltage_mv()` no mesmo ciclo de 100 ms, reduzindo em **~43% o tráfego I2C no `TPS546`**.
* **Tratamento não-fatal de `TPS546_STATUS_CML` ([`main/power/TPS546.c`](../main/power/TPS546.c#L782-L798)):**
  * Em `TPS546_check_status()`, inspecionar separadamente `u16_status & TPS546_STATUS_CML`:
    * Ler `PMBUS_STATUS_CML` (`0x7E`), incrementar `POWER_MANAGEMENT_MODULE.cml_error_count` e emitir `ESP_LOGW`.
    * Enviar `PMBUS_CLEAR_FAULTS` (`0x03`) para destravar o registrador de status sem retornar `ESP_FAIL` (contanto que nenhuma falha elétrica/térmica crítica em `OFF | VOUT_OV | IOUT_OC | VIN_UV | TEMP` esteja presente).
* **Recuperação nativa do barramento I2C ([`main/i2c_bitaxe.c`](../main/i2c_bitaxe.c#L34-L61)):**
  * Em `i2c_transfer_with_retries()`, quando uma transferência falhar antes da última tentativa, invocar `i2c_master_bus_reset(i2c_bus_handle)` (API nativa `driver/i2c_master.h` do ESP-IDF v5.x) para destravar escravos I2C presos em `SDA LOW`.

#### 4.4. Watchdog de `0 GH/s` e Domínios Congelados (3 Níveis na `POWER_MANAGEMENT_task`)
* **Detecção em [`main/tasks/hashrate_monitor_task.c`](../main/tasks/hashrate_monitor_task.c#L182-L198):**
  * **Condições de armação:** `ASIC_initalized == true`, Stratum ativo (`SYSTEM_MODULE.work_received > 0` e `!SYSTEM_MODULE.pools_unavailable`), sem falha térmica/elétrica (`overheat_mode == 0`, `power_fault == 0`, `!hardware_fault`), fora de self-test e após janela de carência de `30 s` pós-inicialização ou transição de frequência.
  * **Critérios de disparo:**
    1. **Hashrate Total Zero:** `hashrate == 0.0` por **`60 s` consecutivos** (4 ciclos de 15 s).
    2. **Domínio Assimétrico Congelado:** qualquer um dos 4 domínios do `BM1370` (`domain_measurements[0][0..3]`) com valor **`< 10%` da média dos demais domínios por 3 ciclos consecutivos (`45 s`)**.
* **Execução síncrona em 3 níveis dentro da [`POWER_MANAGEMENT_task`](../main/tasks/power_management_task.c) (evitando corrida em UART/I2C):**
  * O `hashrate_monitor_task` sinaliza `POWER_MANAGEMENT_MODULE.asic_watchdog_trigger = true` e a `POWER_MANAGEMENT_task` executa a recuperação:
    1. **Nível 1 (1ª falha):** executa re-init suave do ASIC (`mining_stop()` → aguarda `2 s` → `mining_start()`), preservando conexão Wi-Fi/Stratum e estatísticas da sessão, e incrementa `watchdog_resets`.
    2. **Nível 2 (2ª e 3ª falhas em janela `< 10 min`):** aplica *step-down* preventivo de **`-12.5 MHz`** na frequência ativa em RAM (respeitando o piso de `400 MHz`) antes de executar `mining_stop()` → `mining_start()`, estabilizando automaticamente domínios que congelaram por limite de *undervolt/overclock*.
    3. **Nível 3 (4ª falha consecutiva sem estabilização):** aciona `esp_restart()`.

---

### Fase 3 — *Soft-Throttling* Térmico em RAM e Proteção de Emergência Sem Corromper a NVS

#### 4.5. Limites Físicos Rígidos (*Hard Caps*) e Zonas Térmicas

| Grandeza | Início *Soft-Throttle* | Corte de Emergência (*Hard Cap*) | Histerese de Recuperação | Observação |
| :--- | :--- | :--- | :--- | :--- |
| Temperatura ASIC (`BM1370`) | **`68 °C`** (`-25 MHz` se `>= 72 °C`) | **`75 °C`** | **`< 65 °C`** por `30 s` | Mantém diodo externo do `BM1370` legível durante o *throttle* |
| Temperatura VR (`TPS546D24A`) | **`90 °C`** (`-25 MHz` se `>= 98 °C`) | **`105 °C`** | **`< 85 °C`** por `30 s` | Protege o MOSFET integrado do `TPS546D24A` |
| Tensão de Núcleo (`VCore`) | Piso *throttle*: `1000 mV` | **`1000 mV` – `1250 mV`** | — | Inclui compensação de *Vdroop* |
| Frequência do ASIC | Piso *throttle*: `400 MHz` | — | — | Transições em passos suaves |
| Potência Total de Entrada | — | **`32 W`** | — | Margem abaixo dos `40 W` da placa DC 12 V |

#### 4.6. Máquina de Estados Térmica em [`main/tasks/power_management_task.c`](../main/tasks/power_management_task.c#L165-L246)
1. **Separação entre Alvo NVS e Estado Efetivo em RAM:**
   * A NVS define apenas a intenção do usuário (`target_frequency` e `target_voltage`).
   * A operação real usa variáveis em RAM (`actual_frequency`, `actual_voltage`, `throttle_active`).
2. **Zona de Alerta / *Soft-Throttling* (`ASIC >= 68 °C` ou `VR >= 90 °C`):**
   * Ativa `throttle_active = 1` e força a ventoinha a `100%` em RAM (sem alterar `NVS_CONFIG_AUTO_FAN_SPEED`).
   * A cada **`10 s`** acima do limiar, reduz em RAM **`-12.5 MHz`** (ou **`-25 MHz`** se `ASIC >= 72 °C` ou `VR >= 98 °C`) e **`-10 mV`** no `VCore`, até o piso de `400 MHz` / `1000 mV`.
3. **Zona de Recuperação (`ASIC < 65 °C` e `VR < 85 °C` contínuos por `30 s`):**
   * Enquanto `actual_frequency < target_frequency` (ou `actual_voltage < target_voltage`), restaura gradualmente **`+12.5 MHz`** e **`+10 mV`** a cada **`15 s`** até retornar ao alvo do usuário, limpando `throttle_active = 0` e devolvendo o controle normal ao PID/modo da ventoinha.
4. **Zona de Emergência Crítica (`ASIC >= 75 °C` ou `VR >= 105 °C`):**
   * Caso a temperatura continue subindo mesmo após o *Soft-Throttling* (ex.: falha física na ventoinha), executa `mining_stop()`, mantém o fan em `100%` e ativa `overheat_mode = 1` até intervenção do usuário, mas **NUNCA sobrescreve `NVS_CONFIG_ASIC_VOLTAGE`, `NVS_CONFIG_ASIC_FREQUENCY` ou `NVS_CONFIG_AUTO_FAN_SPEED` na NVS**.

---

### Fase 4 — Compensação de *Vdroop*, Perfis DVFS (Auto-Tuner) e Telemetria no `AxeOS`

#### 4.7. Compensação de *Vdroop* em Malha Lenta
* A cada ciclo de estabilização (`10 s`) em operação normal (sem *throttling* e após transição de clock), comparar `VCORE_get_voltage_mv()` com `actual_voltage`.
* Se houver queda resistiva (`Vcore_medido < actual_voltage - 5 mV`), ajustar `vdroop_offset_mv` em passos de `+5 mV` até o **teto máximo de `+20 mV`** (respeitando sempre o limite absoluto de `1250 mV` e `32 W` de potência).

#### 4.8. Perfis de Operação DVFS / Auto-Tuner
Configurável via NVS (`dvfs_profile`, padrão `0 = Manual` ou selecionável pelo usuário), operando sempre dentro dos *Hard Caps* da Seção 4.5:

| Código / Perfil | Objetivo | Faixa de Frequência | Teto de Erro (`errorPercentage`) | Comportamento |
| :--- | :--- | :--- | :--- | :--- |
| **`0` — `Manual`** | Alvo fixo do usuário | Definido pelo usuário | — | Mantém clock/tensão da NVS com *Soft-Throttling*, *Vdroop* e Watchdog ativos |
| **`1` — `Eficiência`** | Menor `J/TH` | `500–600 MHz` | **`<= 0,5%`** | Ajusta `VCore` em passos de `-5 mV` a cada `60 s` até encontrar o ponto mínimo estável |
| **`2` — `Equilibrado`** | Melhor `TH/s` com baixo ruído/temperatura | `525–625 MHz` | **`<= 0,8%`** | Escala clock em passos de `+12.5 MHz` enquanto `ASIC < 64 °C`, `VR < 80 °C` e erro `<= 0,8%` |
| **`3` — `Performance`** | Máximo `TH/s` seguro no envelope | `550–675 MHz` | **`<= 1,0%`** | Escala clock e `VCore` até encontrar o primeiro limite (`68 °C` ASIC, `90 °C` VR, `1250 mV`, `32 W` ou `1,0%` erro) |

#### 4.9. Exposição na API HTTP (`openapi.yaml`) e Frontend Angular (`AxeOS`)
Seguindo rigorosamente as convenções do [`AGENTS.md`](../AGENTS.md) (flags booleanas como `integer` `0/1` no JSON e paridade com mocks):
1. **Backend C ([`main/http_server/system_api_json.c`](../main/http_server/system_api_json.c) e [`main/http_server/http_server.c`](../main/http_server/http_server.c)):**
   * Adicionar ao `GET /api/system/info`:
     * `throttleActive` (`integer`: `0` ou `1` via `cJSON_AddNumberToObject`)
     * `targetFrequency` (`number`: frequência alvo da NVS em MHz, enquanto `frequency` reflete a frequência efetiva atual em RAM)
     * `targetVoltage` (`integer`: tensão alvo da NVS em mV, enquanto `coreVoltage` reflete a tensão comandada atual em RAM)
     * `vdroopOffsetMv` (`integer`: compensação de *Vdroop* aplicada em mV)
     * `dvfsProfile` (`integer`: `0 = Manual`, `1 = Eficiência`, `2 = Equilibrado`, `3 = Performance`)
     * `cmlErrorCount` (`integer`: contador de eventos `STATUS_CML` recuperados)
     * `watchdogResets` (`integer`: contador de recuperações do watchdog de ASIC/domínios na sessão)
   * Adicionar suporte a `dvfsProfile` no `PATCH /api/system`.
2. **OpenAPI & Frontend ([`main/http_server/axe-os/openapi.yaml`](../main/http_server/axe-os/openapi.yaml) e [`system.service.ts`](../main/http_server/axe-os/src/app/services/system.service.ts)):**
   * Atualizar o schema `SystemInfo` e `SystemConfig` no `openapi.yaml`.
   * Executar `npm run generate:api` em `main/http_server/axe-os`.
   * Atualizar os objetos mockados em [`main/http_server/axe-os/src/app/services/system.service.ts`](../main/http_server/axe-os/src/app/services/system.service.ts).
   * Validar testes unitários do frontend com `npm run test:ci`.

---

## 5. Checklist de Execução

- [x] **Fase 0 (Hardware & Config):**
  - [x] Realizar backup completo da Flash (16 MB) e partição NVS (`24 KB`) via USB-C antes de gravar no hardware físico.
  - [x] Adicionar `FAMILY_GAMMA_V2` e `FAMILY_GAMMA_DUO` em `default_families[]` e `"601a-4"` em `default_configs[]` ([`main/device_config.h`](../main/device_config.h)).
  - [x] Implementar fallback por prefixo `"601a*"` e alias `"LT-001"` em [`device_config_init`](../main/device_config.c).
- [x] **Fase 1 (SRAM Interna, Core Pinning & Pipeline de Jobs):**
  - [x] Migrar stacks elegíveis (`POWER_MANAGEMENT`, `FAN_CONTROLLER`, `create_jobs`, `ASIC_result`, `statistics`, `hashrate_monitor`, `websocket_log` e tasks `BAP`) para PSRAM com `esp_psram_is_initialized()`, mantendo `nvs_task` e `httpd` em SRAM interna.
  - [x] Migrar `rest_server_context_t` (`10,5 KB`), `scratch_buf` do WebSocket (`4 KB`), `rx_buf` do BAP (`1 KB`), `bm_job` (`~10 KB`) e buffers/structs `mining_notify` do Stratum (`~6 KB`) para PSRAM.
  - [x] Aplicar *Core Pinning*: Core 1 (`ASIC_result_task`, `create_jobs_task`, `hashrate_monitor_task`) e Core 0 (Rede, `httpd`, Stratum, Power, Fan, Stats, NVS, BAP).
  - [x] Corrigir Use-After-Free em [`stratum_v1_task.c`](../main/tasks/stratum_v1_task.c), race condition de `active_jobs`/`valid_jobs_lock` em [`bm1370.c`](../components/asic/bm1370.c), reset indevido de hashrate em [`system.c`](../main/system.c) e cálculo de deadline em [`work_queue.c`](../main/work_queue.c).
- [x] **Fase 2 (I2C, `TPS546 STATUS_CML` & Watchdog em 3 Níveis):**
  - [x] Cachear `PMBUS_VOUT_MODE` no `TPS546_init()` e eliminar leitura dupla de `TPS546_get_vout()` a cada 100 ms.
  - [x] Tratar `TPS546_STATUS_CML` com log, contador `cml_error_count` e `PMBUS_CLEAR_FAULTS` sem derrubar a mineração.
  - [x] Adicionar `i2c_master_bus_reset()` nos retries de [`i2c_bitaxe.c`](../main/i2c_bitaxe.c).
  - [x] Implementar Watchdog de `0 GH/s` (`60 s`) e domínios congelados (`< 10%` por `45 s`) com recuperação em 3 níveis na `POWER_MANAGEMENT_task` (1º re-init normal; 2º/3º re-init com `-12.5 MHz` em RAM; 4º `esp_restart()`).
- [x] **Fase 3 (*Soft-Throttling* Térmico em RAM & Emergência Segura):**
  - [x] Implementar *Soft-Throttling* progressivo em RAM (`>= 68 °C` ASIC ou `>= 90 °C` VR) e rampa de recuperação com histerese (`< 65 °C` ASIC e `< 85 °C` VR por `30 s`).
  - [x] Garantir que a parada de emergência crítica (`>= 75 °C` ASIC ou `>= 105 °C` VR) acione `mining_stop()` e `overheat_mode = 1` **sem sobrescrever** tensão, frequência e modo do fan na NVS.
- [ ] **Fase 4 (*Vdroop*, Auto-Tuner DVFS & API/UI `AxeOS`):**
  - [ ] Implementar compensação de *Vdroop* em malha lenta (teto `+20 mV`) e perfis DVFS (`Manual`, `Eficiência`, `Equilibrado`, `Performance`).
  - [ ] Atualizar [`system_api_json.c`](../main/http_server/system_api_json.c), [`http_server.c`](../main/http_server/http_server.c), [`openapi.yaml`](../main/http_server/axe-os/openapi.yaml), regenerar API (`npm run generate:api`), atualizar mocks em [`system.service.ts`](../main/http_server/axe-os/src/app/services/system.service.ts) e validar com `npm run test:ci` e `idf.py build`.
