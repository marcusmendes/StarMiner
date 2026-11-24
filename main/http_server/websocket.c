#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_http_server.h"
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "websocket.h"
#include "websocket_log.h"
#include "websocket_api.h"
#include "http_server.h"
#include "log_buffer.h"

#define WS_LOG_SCRATCH_SIZE 2048

static const char * TAG = "websocket";

typedef struct {
    int fd;
    uint32_t type;
} ws_client_t;

typedef struct {
    WebSocketClientType client_type;
    int target_fd;
    httpd_ws_type_t frame_type;
    uint8_t *payload;
    size_t len;
} ws_work_item_t;

static ws_client_t clients[MAX_WEBSOCKET_CLIENTS];
static int type_counts[WS_TYPE_MAX] = {0};
static int s_pending_work[WS_TYPE_MAX] = {0};
static SemaphoreHandle_t clients_mutex = NULL;
static portMUX_TYPE s_clients_mutex_init_lock = portMUX_INITIALIZER_UNLOCKED;
static httpd_handle_t server_handle = NULL;
static TaskHandle_t s_websocket_log_task_handle = NULL;

static ws_queue_work_fn_t s_queue_work_fn = httpd_queue_work;
#ifdef CONFIG_HTTPD_WS_SUPPORT
static ws_send_frame_fn_t s_send_frame_fn = httpd_ws_send_frame_async;
#else
static esp_err_t ws_unsupported_send_frame(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame)
{
    (void)hd;
    (void)fd;
    (void)frame;
    return ESP_ERR_NOT_SUPPORTED;
}
static ws_send_frame_fn_t s_send_frame_fn = ws_unsupported_send_frame;
#endif

static bool ensure_clients_mutex(void)
{
    if (clients_mutex != NULL) {
        return true;
    }

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_clients_mutex_init_lock);
    if (clients_mutex == NULL) {
        clients_mutex = created;
        created = NULL;
    }
    portEXIT_CRITICAL(&s_clients_mutex_init_lock);

    if (created != NULL) {
        vSemaphoreDelete(created);
    }

    return clients_mutex != NULL;
}

static int ws_max_pending_for_type(WebSocketClientType type)
{
    switch (type) {
        case WS_TYPE_LOGS:
            return WS_MAX_PENDING_LOGS;
        case WS_TYPE_API:
            return WS_MAX_PENDING_API;
        default:
            return 0;
    }
}

static uint8_t *ws_alloc_payload_copy(const uint8_t *src, size_t len)
{
    if (len == 0) {
        return NULL;
    }

    uint8_t *copy = NULL;
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        copy = (uint8_t *)heap_caps_malloc(len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
    if (copy == NULL) {
        copy = (uint8_t *)malloc(len);
    }
    if (copy != NULL) {
        memcpy(copy, src, len);
    }
    return copy;
}

static void ws_decrement_pending(WebSocketClientType type)
{
    if (type < 0 || type >= WS_TYPE_MAX || !ensure_clients_mutex()) {
        return;
    }
    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (s_pending_work[type] > 0) {
            s_pending_work[type]--;
        }
        xSemaphoreGive(clients_mutex);
    }
}

static void ws_send_work_fn(void *arg)
{
    ws_work_item_t *item = (ws_work_item_t *)arg;
    if (item == NULL) {
        return;
    }

    int target_fds[MAX_WEBSOCKET_CLIENTS];
    int target_count = 0;
    httpd_handle_t srv = NULL;
    ws_send_frame_fn_t send_fn = NULL;

    if (ensure_clients_mutex() && xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        if (item->client_type >= 0 && item->client_type < WS_TYPE_MAX &&
            s_pending_work[item->client_type] > 0) {
            s_pending_work[item->client_type]--;
        }

        srv = server_handle;
        send_fn = s_send_frame_fn;

        if (srv != NULL && send_fn != NULL) {
            for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
                if (clients[i].fd == -1) {
                    continue;
                }
                if (item->target_fd >= 0) {
                    if (clients[i].fd == item->target_fd) {
                        target_fds[target_count++] = clients[i].fd;
                        break;
                    }
                } else if ((WebSocketClientType)clients[i].type == item->client_type) {
                    target_fds[target_count++] = clients[i].fd;
                }
            }
        }

        xSemaphoreGive(clients_mutex);
    }

    if (srv != NULL && send_fn != NULL) {
        for (int i = 0; i < target_count; i++) {
            httpd_ws_frame_t pkt = {
                .final = true,
                .fragmented = false,
                .type = item->frame_type,
                .payload = item->payload,
                .len = item->len,
            };
            if (send_fn(srv, target_fds[i], &pkt) != ESP_OK) {
                ESP_LOGW(TAG, "Failed to send WebSocket frame to fd: %d", target_fds[i]);
            }
        }
    }

    free(item->payload);
    free(item);
}

static void websocket_enqueue_frame(WebSocketClientType req_type, int target_fd, const httpd_ws_frame_t *pkt)
{
    if (pkt == NULL || (pkt->len > 0 && pkt->payload == NULL)) {
        return;
    }
    if (target_fd < 0 && (req_type < 0 || req_type >= WS_TYPE_MAX)) {
        return;
    }
    if (!ensure_clients_mutex()) {
        return;
    }

    WebSocketClientType resolved_type = req_type;
    httpd_handle_t srv = NULL;
    ws_queue_work_fn_t queue_fn = NULL;
    bool can_enqueue = false;

    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    if (server_handle != NULL && s_queue_work_fn != NULL) {
        bool has_target = false;
        if (target_fd >= 0) {
            for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
                if (clients[i].fd == target_fd) {
                    resolved_type = (WebSocketClientType)clients[i].type;
                    has_target = (resolved_type >= 0 && resolved_type < WS_TYPE_MAX);
                    break;
                }
            }
        } else {
            has_target = (type_counts[resolved_type] > 0);
        }

        if (has_target) {
            int max_pending = ws_max_pending_for_type(resolved_type);
            if (s_pending_work[resolved_type] < max_pending) {
                s_pending_work[resolved_type]++;
                srv = server_handle;
                queue_fn = s_queue_work_fn;
                can_enqueue = true;
            }
        }
    }

    xSemaphoreGive(clients_mutex);

    if (!can_enqueue) {
        return;
    }

    ws_work_item_t *item = (ws_work_item_t *)calloc(1, sizeof(ws_work_item_t));
    if (item == NULL) {
        ws_decrement_pending(resolved_type);
        ESP_LOGW(TAG, "Failed to allocate WebSocket work item");
        return;
    }

    item->client_type = resolved_type;
    item->target_fd = target_fd;
    item->frame_type = pkt->type;
    item->len = pkt->len;
    item->payload = ws_alloc_payload_copy(pkt->payload, pkt->len);
    if (pkt->len > 0 && item->payload == NULL) {
        free(item);
        ws_decrement_pending(resolved_type);
        ESP_LOGW(TAG, "Failed to allocate WebSocket payload copy (%zu bytes)", pkt->len);
        return;
    }

    esp_err_t q_err = queue_fn(srv, ws_send_work_fn, item);
    if (q_err != ESP_OK) {
        free(item->payload);
        free(item);
        ws_decrement_pending(resolved_type);
        ESP_LOGW(TAG, "Failed to queue WebSocket work (%s)", esp_err_to_name(q_err));
    }
}

void websocket_set_io_hooks(ws_queue_work_fn_t queue_work_fn, ws_send_frame_fn_t send_frame_fn)
{
    if (!ensure_clients_mutex()) {
        return;
    }
    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    s_queue_work_fn = (queue_work_fn != NULL) ? queue_work_fn : httpd_queue_work;
#ifdef CONFIG_HTTPD_WS_SUPPORT
    s_send_frame_fn = (send_frame_fn != NULL) ? send_frame_fn : httpd_ws_send_frame_async;
#else
    s_send_frame_fn = (send_frame_fn != NULL) ? send_frame_fn : ws_unsupported_send_frame;
#endif

    xSemaphoreGive(clients_mutex);
}

int websocket_get_pending_work_count(WebSocketClientType type)
{
    if (type < 0 || type >= WS_TYPE_MAX || !ensure_clients_mutex()) {
        return 0;
    }
    int count = 0;
    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = s_pending_work[type];
        xSemaphoreGive(clients_mutex);
    }
    return count;
}

void websocket_set_log_task_handle(TaskHandle_t task_handle)
{
    __atomic_store_n(&s_websocket_log_task_handle, task_handle, __ATOMIC_RELAXED);
}

int websocket_get_active_client_count(WebSocketClientType type)
{
    if (type < 0 || type >= WS_TYPE_MAX || !ensure_clients_mutex()) {
        return 0;
    }
    int count = 0;
    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        count = type_counts[type];
        xSemaphoreGive(clients_mutex);
    }
    return count;
}

void __attribute__((weak)) websocket_log_notify(void)
{
    TaskHandle_t task_handle = __atomic_load_n(&s_websocket_log_task_handle, __ATOMIC_RELAXED);
    if (task_handle != NULL && __atomic_load_n(&type_counts[WS_TYPE_LOGS], __ATOMIC_RELAXED) > 0) {
        xTaskNotifyGive(task_handle);
    }
}

esp_err_t websocket_add_client(int fd, WebSocketClientType type)
{
    if (fd < 0 || type < 0 || type >= WS_TYPE_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ensure_clients_mutex()) {
        return ESP_ERR_NO_MEM;
    }
    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex for adding client");
        return ESP_FAIL;
    }

    esp_err_t ret = ESP_FAIL;
    int added_slot = -1;
    bool duplicate = false;
    for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            duplicate = true;
            break;
        }
        if (clients[i].fd == -1 && added_slot == -1) {
            added_slot = i;
        }
    }

    if (!duplicate && added_slot != -1) {
        clients[added_slot].fd = fd;
        clients[added_slot].type = type;
        __atomic_add_fetch(&type_counts[type], 1, __ATOMIC_RELAXED);
        ret = ESP_OK;
    }

    xSemaphoreGive(clients_mutex);

    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Added WebSocket %s client, fd: %d, slot: %d",
                 type == WS_TYPE_LOGS ? "log" : "api", fd, added_slot);
        TaskHandle_t task_handle = __atomic_load_n(&s_websocket_log_task_handle, __ATOMIC_RELAXED);
        if (type == WS_TYPE_LOGS && task_handle != NULL) {
            xTaskNotifyGive(task_handle);
        }
    } else if (duplicate) {
        ESP_LOGW(TAG, "WebSocket client fd: %d already registered", fd);
    } else {
        ESP_LOGE(TAG, "Max WebSocket clients reached, cannot add fd: %d", fd);
    }

    return ret;
}

void websocket_remove_client(int fd)
{
    if (fd < 0 || !ensure_clients_mutex()) {
        return;
    }
    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGE(TAG, "Failed to acquire mutex for removing client");
        return;
    }

    bool removed = false;
    int removed_slot = -1;
    WebSocketClientType removed_type = WS_TYPE_LOGS;

    for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
        if (clients[i].fd == fd) {
            removed_type = (WebSocketClientType)clients[i].type;
            clients[i].fd = -1;
            clients[i].type = 0;
            if (removed_type >= 0 && removed_type < WS_TYPE_MAX && type_counts[removed_type] > 0) {
                __atomic_sub_fetch(&type_counts[removed_type], 1, __ATOMIC_RELAXED);
            }
            removed = true;
            removed_slot = i;
            break;
        }
    }

    xSemaphoreGive(clients_mutex);

    if (removed) {
        ESP_LOGI(TAG, "Removed WebSocket %s client, fd: %d, slot: %d",
                 removed_type == WS_TYPE_LOGS ? "log" : "api", fd, removed_slot);
    }
}

void websocket_send_to_client(int fd, httpd_ws_frame_t *pkt)
{
    if (fd < 0) {
        return;
    }
    websocket_enqueue_frame(WS_TYPE_MAX, fd, pkt);
}

void websocket_broadcast(WebSocketClientType type, httpd_ws_frame_t *pkt)
{
    websocket_enqueue_frame(type, -1, pkt);
}

void websocket_close_fn(httpd_handle_t hd, int fd)
{
    (void)hd;
    websocket_remove_client(fd);
    close(fd);
}

void websocket_init(httpd_handle_t server)
{
    if (!ensure_clients_mutex()) {
        return;
    }
    if (xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    server_handle = server;
    for (int i = 0; i < MAX_WEBSOCKET_CLIENTS; i++) {
        clients[i].fd = -1;
        clients[i].type = 0;
    }
    for (int i = 0; i < WS_TYPE_MAX; i++) {
        __atomic_store_n(&type_counts[i], 0, __ATOMIC_RELAXED);
        s_pending_work[i] = 0;
    }

    s_queue_work_fn = httpd_queue_work;
#ifdef CONFIG_HTTPD_WS_SUPPORT
    s_send_frame_fn = httpd_ws_send_frame_async;
#else
    s_send_frame_fn = ws_unsupported_send_frame;
#endif

    xSemaphoreGive(clients_mutex);
}

esp_err_t websocket_handler(httpd_req_t *req)
{
    // Detect handshake by checking for the "Upgrade" header
    char upgrade_hdr[16];
    if (httpd_req_get_hdr_value_str(req, "Upgrade", upgrade_hdr, sizeof(upgrade_hdr)) == ESP_OK &&
        strcasecmp(upgrade_hdr, "websocket") == 0) {

        if (is_network_allowed(req) != ESP_OK) {
            return httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Unauthorized");
        }

        int active_clients = 0;
        if (ensure_clients_mutex() && xSemaphoreTake(clients_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            for (int i = 0; i < WS_TYPE_MAX; i++) {
                active_clients += type_counts[i];
            }
            xSemaphoreGive(clients_mutex);
        } else {
            ESP_LOGE(TAG, "Failed to acquire mutex checking active WebSocket clients");
            return ESP_FAIL;
        }

        if (active_clients >= MAX_WEBSOCKET_CLIENTS) {
            ESP_LOGE(TAG, "Max WebSocket clients reached, rejecting new connection");
            return httpd_resp_send_custom_err(req, "429 Too Many Requests", "Max WebSocket clients reached");
        }

        WebSocketClientType type = (WebSocketClientType)(uintptr_t)req->user_ctx;
        int fd = httpd_req_to_sockfd(req);
        if (websocket_add_client(fd, type) != ESP_OK) {
            ESP_LOGE(TAG, "Unexpected failure adding client, fd: %d", fd);
            return ESP_FAIL;
        }

        if (type == WS_TYPE_API) {
            websocket_api_on_connect(fd);
        }

        return ESP_OK;
    }

#ifdef CONFIG_HTTPD_WS_SUPPORT
    // Handle WebSocket frame
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));

    // Get frame header to allow ESP-IDF to handle control frames (Ping/Pong/Close)
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        return ret;
    }

    // If there's a payload, drain it
    if (ws_pkt.len > 0) {
        uint8_t *buf = (uint8_t *)calloc(1, ws_pkt.len + 1);
        if (buf) {
            ws_pkt.payload = buf;
            ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
            free(buf);
            return ret;
        }
    }

    return ESP_OK;
#else
    return ESP_ERR_NOT_SUPPORTED;
#endif
}
