#ifndef WEBSOCKET_H_
#define WEBSOCKET_H_

#include "sdkconfig.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#ifndef CONFIG_HTTPD_WS_SUPPORT
typedef enum {
    HTTPD_WS_TYPE_CONTINUE   = 0x0,
    HTTPD_WS_TYPE_TEXT       = 0x1,
    HTTPD_WS_TYPE_BINARY     = 0x2,
    HTTPD_WS_TYPE_CLOSE      = 0x8,
    HTTPD_WS_TYPE_PING       = 0x9,
    HTTPD_WS_TYPE_PONG       = 0xA
} httpd_ws_type_t;

typedef struct httpd_ws_frame {
    bool final;
    bool fragmented;
    httpd_ws_type_t type;
    uint8_t *payload;
    size_t len;
} httpd_ws_frame_t;
#endif

void websocket_set_log_task_handle(TaskHandle_t task_handle);

#define MESSAGE_QUEUE_SIZE (128)
#define MAX_WEBSOCKET_CLIENTS (10)
#define WS_MAX_PENDING_LOGS (8)
#define WS_MAX_PENDING_API  (4)
#ifndef LOG_BUFFER_SIZE
#define LOG_BUFFER_SIZE  (512 * 1024)  /* 512 KB */
#endif

typedef enum {
    WS_TYPE_LOGS,
    WS_TYPE_API,
    WS_TYPE_MAX
} WebSocketClientType;

typedef esp_err_t (*ws_queue_work_fn_t)(httpd_handle_t handle, httpd_work_fn_t work, void *arg);
typedef esp_err_t (*ws_send_frame_fn_t)(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame);

void websocket_set_io_hooks(ws_queue_work_fn_t queue_work_fn, ws_send_frame_fn_t send_frame_fn);
int websocket_get_pending_work_count(WebSocketClientType type);

esp_err_t websocket_add_client(int fd, WebSocketClientType type);
void websocket_remove_client(int fd);
void websocket_broadcast(WebSocketClientType type, httpd_ws_frame_t *pkt);
void websocket_send_to_client(int fd, httpd_ws_frame_t *pkt);
int websocket_get_active_client_count(WebSocketClientType type);
void websocket_init(httpd_handle_t server);
esp_err_t websocket_handler(httpd_req_t *req);

void websocket_close_fn(httpd_handle_t hd, int sockfd);

/**
 * Notifies the WebSocket task that new log data is available in the ring buffer.
 * Non-blocking, safe to call from log hook.
 */
void websocket_log_notify(void);

#endif /* WEBSOCKET_H_ */
