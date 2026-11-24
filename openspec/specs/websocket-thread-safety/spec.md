# websocket-thread-safety Specification

## Purpose

Garante a segurança concorrente (*thread-safety*) do subsistema WebSocket do servidor HTTP embarcado, serializando transmissões na thread do `httpd`, protegendo o registro de clientes e limitando filas pendentes sob carga.

## Requirements

### Requirement: Serialização de envios WebSocket na thread do servidor HTTP
Todas as transmissões de frames WebSocket (`websocket_broadcast` e `websocket_send_to_client` em `main/http_server/websocket.c`) SHALL ser despachadas para execução exclusiva na thread do servidor HTTP via `httpd_queue_work()`, utilizando cópia própria do payload liberada apenas após o envio ou em falha de enfileiramento.

#### Scenario: Envio concorrente a partir de task externa e conexão inicial no httpd
- **WHEN** `websocket_api_task` ou `websocket_log_task` dispara `websocket_broadcast()` ao mesmo tempo em que um novo cliente conecta em `websocket_handler()` e chama `websocket_api_on_connect(fd)`
- **THEN** ambas as transmissões executam serializadas na thread do `httpd` via `httpd_queue_work()`, sem chamadas concorrentes a `httpd_ws_send_frame_async()` fora do loop do servidor HTTP

#### Scenario: Falha ao enfileirar trabalho no servidor HTTP
- **WHEN** `httpd_queue_work()` retorna erro ou o servidor HTTP ainda não está inicializado
- **THEN** a função de envio descarta a operação com segurança, decrementa contadores pendentes e libera imediatamente o buffer alocado do payload e a estrutura de trabalho

### Requirement: Sincronização da tabela de clientes e revalidação de socket
Todo acesso de leitura e escrita a `clients[]` e `type_counts[]` em `main/http_server/websocket.c` SHALL ocorrer sob `clients_mutex`, e o callback executado pelo `httpd` SHALL copiar os `fd`s ativos elegíveis e revalidar que o socket permanece registrado antes de invocar `httpd_ws_send_frame_async()`.

#### Scenario: Desconexão abrupta de cliente enquanto um frame está na fila
- **WHEN** um cliente WebSocket fecha a conexão (`websocket_close_fn` remove o `fd` sob `clients_mutex`) depois que um broadcast ou envio direcionado foi enfileirado em `httpd_queue_work()`
- **THEN** o callback na thread `httpd` verifica que o `fd` não está mais presente em `clients[]` e ignora o envio para aquele descritor sem acessar socket inválido ou manter `clients_mutex` retido durante E/S de rede

### Requirement: Controle de backpressure por tipo de canal WebSocket
O subsistema WebSocket SHALL manter contadores de envios pendentes por tipo (`WS_TYPE_LOGS` e `WS_TYPE_API`) com limites máximos configurados (máximo `8` itens pendentes para logs e `4` para API), descartando novos envios excedentes sem vazamento de memória quando o servidor HTTP estiver congestionado.

#### Scenario: Saturação transitória da fila de trabalho do httpd sob rajada de logs
- **WHEN** `websocket_log_task` tenta enviar novos blocos enquanto já existem `8` itens `WS_TYPE_LOGS` pendentes na fila do `httpd`
- **THEN** o envio excedente é descartado antes de alocar ou enfileirar novo item em `httpd_queue_work()`, preservando a estabilidade de memória e permitindo que transmissões `WS_TYPE_API` mantenham seu orçamento independente

### Requirement: Eliminação de estado mutável compartilhado sem trava em websocket_api
Em `main/http_server/websocket_api.c`, o cálculo de pré-alocação de serialização JSON SHALL evitar estado estático compartilhado mutável sem sincronização entre `websocket_api_on_connect()` (thread `httpd`) e `websocket_api_task()` (task dedicada).

#### Scenario: Conexão simultânea de cliente durante atualização periódica da API
- **WHEN** `websocket_api_on_connect()` serializa o snapshot inicial no contexto do `httpd` simultaneamente a um ciclo de `process_and_send_update()` em `websocket_api_task()`
- **THEN** nenhuma variável estática mutável (como `prebuffer_len`) é lida ou escrita concorrentemente sem proteção entre as duas threads
