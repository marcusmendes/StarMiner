#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "unity.h"
#include "websocket.h"

esp_err_t __attribute__((weak)) is_network_allowed(httpd_req_t *req)
{
    (void)req;
    return ESP_OK;
}

void __attribute__((weak)) websocket_api_on_connect(int fd)
{
    (void)fd;
}

#define TEST_MAX_QUEUED_WORKS 32
#define TEST_MAX_SENT_FRAMES  64
#define TEST_MAX_FRAME_BYTES  256

typedef struct {
    httpd_work_fn_t fn;
    void *arg;
} test_queued_work_t;

typedef struct {
    int fd;
    httpd_ws_type_t type;
    bool final;
    size_t len;
    char data[TEST_MAX_FRAME_BYTES];
} test_sent_frame_t;

static test_queued_work_t s_queued_works[TEST_MAX_QUEUED_WORKS];
static int s_queued_count = 0;
static test_sent_frame_t s_sent_frames[TEST_MAX_SENT_FRAMES];
static int s_sent_count = 0;
static esp_err_t s_fake_queue_ret = ESP_OK;

static void reset_test_harness(void)
{
    memset(s_queued_works, 0, sizeof(s_queued_works));
    s_queued_count = 0;
    memset(s_sent_frames, 0, sizeof(s_sent_frames));
    s_sent_count = 0;
    s_fake_queue_ret = ESP_OK;
}

static esp_err_t fake_queue_work(httpd_handle_t handle, httpd_work_fn_t work, void *arg)
{
    (void)handle;
    if (s_fake_queue_ret != ESP_OK) {
        return s_fake_queue_ret;
    }
    if (s_queued_count >= TEST_MAX_QUEUED_WORKS) {
        return ESP_FAIL;
    }
    s_queued_works[s_queued_count].fn = work;
    s_queued_works[s_queued_count].arg = arg;
    s_queued_count++;
    return ESP_OK;
}

static esp_err_t fake_send_frame(httpd_handle_t hd, int fd, httpd_ws_frame_t *frame)
{
    (void)hd;
    if (s_sent_count < TEST_MAX_SENT_FRAMES && frame != NULL) {
        s_sent_frames[s_sent_count].fd = fd;
        s_sent_frames[s_sent_count].type = frame->type;
        s_sent_frames[s_sent_count].final = frame->final;
        s_sent_frames[s_sent_count].len = frame->len;
        size_t copy_len = (frame->len < TEST_MAX_FRAME_BYTES - 1) ? frame->len : (TEST_MAX_FRAME_BYTES - 1);
        if (copy_len > 0 && frame->payload != NULL) {
            memcpy(s_sent_frames[s_sent_count].data, frame->payload, copy_len);
        }
        s_sent_frames[s_sent_count].data[copy_len] = '\0';
        s_sent_count++;
    }
    return ESP_OK;
}

static void drain_queued_works(void)
{
    for (int i = 0; i < s_queued_count; i++) {
        if (s_queued_works[i].fn != NULL) {
            s_queued_works[i].fn(s_queued_works[i].arg);
        }
    }
    s_queued_count = 0;
}

static httpd_ws_frame_t make_text_frame(const char *msg)
{
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.final = true;
    pkt.type = HTTPD_WS_TYPE_TEXT;
    pkt.payload = (uint8_t *)msg;
    pkt.len = strlen(msg);
    return pkt;
}

TEST_CASE("websocket revalidates disconnected fd in httpd worker before sending", "[websocket]")
{
    websocket_init((httpd_handle_t)0x1);
    reset_test_harness();
    websocket_set_io_hooks(fake_queue_work, fake_send_frame);

    TEST_ASSERT_EQUAL(ESP_OK, websocket_add_client(10, WS_TYPE_API));
    TEST_ASSERT_EQUAL(ESP_OK, websocket_add_client(11, WS_TYPE_API));

    httpd_ws_frame_t pkt = make_text_frame("hello-api");
    websocket_broadcast(WS_TYPE_API, &pkt);
    TEST_ASSERT_EQUAL_INT(1, s_queued_count);

    /* Client 10 disconnects before httpd worker drains the queued frame */
    websocket_remove_client(10);
    drain_queued_works();

    TEST_ASSERT_EQUAL_INT(1, s_sent_count);
    TEST_ASSERT_EQUAL_INT(11, s_sent_frames[0].fd);
    TEST_ASSERT_TRUE(s_sent_frames[0].final);
    TEST_ASSERT_EQUAL_STRING("hello-api", s_sent_frames[0].data);

    /* Targeted send whose fd disconnects before worker execution must send 0 frames */
    httpd_ws_frame_t direct_pkt = make_text_frame("direct-msg");
    websocket_send_to_client(11, &direct_pkt);
    TEST_ASSERT_EQUAL_INT(1, s_queued_count);

    websocket_remove_client(11);
    drain_queued_works();

    TEST_ASSERT_EQUAL_INT(1, s_sent_count);
    TEST_ASSERT_EQUAL_INT(0, websocket_get_pending_work_count(WS_TYPE_API));

    websocket_init(NULL);
}

TEST_CASE("websocket enforces independent backpressure limits (8 logs, 4 api)", "[websocket]")
{
    websocket_init((httpd_handle_t)0x1);
    reset_test_harness();
    websocket_set_io_hooks(fake_queue_work, fake_send_frame);

    TEST_ASSERT_EQUAL(ESP_OK, websocket_add_client(20, WS_TYPE_LOGS));
    TEST_ASSERT_EQUAL(ESP_OK, websocket_add_client(21, WS_TYPE_API));

    httpd_ws_frame_t log_pkt = make_text_frame("log-line-payload");
    httpd_ws_frame_t api_pkt = make_text_frame("{\"event\":\"update\"}");

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    for (int i = 0; i < 10; i++) {
        websocket_broadcast(WS_TYPE_LOGS, &log_pkt);
    }
    TEST_ASSERT_EQUAL_INT(WS_MAX_PENDING_LOGS, websocket_get_pending_work_count(WS_TYPE_LOGS));
    TEST_ASSERT_EQUAL_INT(WS_MAX_PENDING_LOGS, s_queued_count);

    /* API channel keeps its own independent budget while LOGS is saturated */
    for (int i = 0; i < 6; i++) {
        websocket_broadcast(WS_TYPE_API, &api_pkt);
    }
    TEST_ASSERT_EQUAL_INT(WS_MAX_PENDING_API, websocket_get_pending_work_count(WS_TYPE_API));
    TEST_ASSERT_EQUAL_INT(WS_MAX_PENDING_LOGS + WS_MAX_PENDING_API, s_queued_count);

    drain_queued_works();

    TEST_ASSERT_EQUAL_INT(0, websocket_get_pending_work_count(WS_TYPE_LOGS));
    TEST_ASSERT_EQUAL_INT(0, websocket_get_pending_work_count(WS_TYPE_API));
    TEST_ASSERT_EQUAL_INT(WS_MAX_PENDING_LOGS + WS_MAX_PENDING_API, s_sent_count);

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)free_before, (uint32_t)free_after);

    websocket_init(NULL);
}

TEST_CASE("websocket frees payload and decrements pending count when httpd_queue_work fails", "[websocket]")
{
    websocket_init((httpd_handle_t)0x1);
    reset_test_harness();
    websocket_set_io_hooks(fake_queue_work, fake_send_frame);

    TEST_ASSERT_EQUAL(ESP_OK, websocket_add_client(30, WS_TYPE_LOGS));
    TEST_ASSERT_EQUAL(ESP_OK, websocket_add_client(31, WS_TYPE_API));

    httpd_ws_frame_t log_pkt = make_text_frame("failed-log-payload-0123456789abcdef");
    httpd_ws_frame_t api_pkt = make_text_frame("failed-api-payload-0123456789abcdef");

    s_fake_queue_ret = ESP_FAIL;

    /* Warm up log path once before heap measurement */
    websocket_broadcast(WS_TYPE_LOGS, &log_pkt);

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_8BIT);

    websocket_broadcast(WS_TYPE_LOGS, &log_pkt);
    websocket_send_to_client(31, &api_pkt);

    TEST_ASSERT_EQUAL_INT(0, websocket_get_pending_work_count(WS_TYPE_LOGS));
    TEST_ASSERT_EQUAL_INT(0, websocket_get_pending_work_count(WS_TYPE_API));
    TEST_ASSERT_EQUAL_INT(0, s_queued_count);
    TEST_ASSERT_EQUAL_INT(0, s_sent_count);

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)free_before, (uint32_t)free_after);

    websocket_init(NULL);
}

typedef struct {
    int base_fd;
    WebSocketClientType type;
    int iterations;
    SemaphoreHandle_t done_sem;
} ws_client_worker_ctx_t;

static void ws_client_churn_task(void *arg)
{
    ws_client_worker_ctx_t *ctx = (ws_client_worker_ctx_t *)arg;
    for (int i = 0; i < ctx->iterations; i++) {
        for (int j = 0; j < 4; j++) {
            (void)websocket_add_client(ctx->base_fd + j, ctx->type);
        }
        taskYIELD();
        for (int j = 0; j < 4; j++) {
            websocket_remove_client(ctx->base_fd + j);
        }
        vTaskDelay(1);
    }
    xSemaphoreGive(ctx->done_sem);
    vTaskDelete(NULL);
}

TEST_CASE("websocket concurrent add/remove client under clients_mutex maintains consistent counts", "[websocket]")
{
    websocket_init((httpd_handle_t)0x1);
    reset_test_harness();
    websocket_set_io_hooks(fake_queue_work, fake_send_frame);

    ws_client_worker_ctx_t worker_a = {
        .base_fd = 100,
        .type = WS_TYPE_LOGS,
        .iterations = 30,
        .done_sem = xSemaphoreCreateBinary(),
    };
    ws_client_worker_ctx_t worker_b = {
        .base_fd = 200,
        .type = WS_TYPE_API,
        .iterations = 30,
        .done_sem = xSemaphoreCreateBinary(),
    };
    TEST_ASSERT_NOT_NULL(worker_a.done_sem);
    TEST_ASSERT_NOT_NULL(worker_b.done_sem);

    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(ws_client_churn_task, "ws_churn_a", 4096, &worker_a, 5, NULL));
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(ws_client_churn_task, "ws_churn_b", 4096, &worker_b, 5, NULL));

    httpd_ws_frame_t log_pkt = make_text_frame("concurrent-log");
    httpd_ws_frame_t api_pkt = make_text_frame("concurrent-api");

    for (int i = 0; i < 40; i++) {
        int logs_n = websocket_get_active_client_count(WS_TYPE_LOGS);
        int api_n = websocket_get_active_client_count(WS_TYPE_API);
        TEST_ASSERT_TRUE(logs_n >= 0 && logs_n <= 4);
        TEST_ASSERT_TRUE(api_n >= 0 && api_n <= 4);
        websocket_broadcast(WS_TYPE_LOGS, &log_pkt);
        websocket_broadcast(WS_TYPE_API, &api_pkt);
        drain_queued_works();
        vTaskDelay(1);
    }

    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(worker_a.done_sem, pdMS_TO_TICKS(5000)));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(worker_b.done_sem, pdMS_TO_TICKS(5000)));
    drain_queued_works();

    TEST_ASSERT_EQUAL_INT(0, websocket_get_active_client_count(WS_TYPE_LOGS));
    TEST_ASSERT_EQUAL_INT(0, websocket_get_active_client_count(WS_TYPE_API));
    TEST_ASSERT_EQUAL_INT(0, websocket_get_pending_work_count(WS_TYPE_LOGS));
    TEST_ASSERT_EQUAL_INT(0, websocket_get_pending_work_count(WS_TYPE_API));

    vSemaphoreDelete(worker_a.done_sem);
    vSemaphoreDelete(worker_b.done_sem);
    websocket_init(NULL);
}
