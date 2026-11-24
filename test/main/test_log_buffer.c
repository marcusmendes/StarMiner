#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "log_buffer.h"
#include "unity.h"

static const char *TEST_TAG = "test_log_buf";
static volatile int s_ws_notify_count = 0;

void websocket_log_notify(void)
{
    s_ws_notify_count++;
}

typedef struct {
    SemaphoreHandle_t done_sem;
    uint64_t total_written;
} mutex_probe_ctx_t;

static void mutex_probe_task(void *arg)
{
    mutex_probe_ctx_t *ctx = (mutex_probe_ctx_t *)arg;
    ctx->total_written = log_buffer_get_total_written();
    xSemaphoreGive(ctx->done_sem);
    vTaskDelete(NULL);
}

TEST_CASE("log_buffer writes short line and releases mutex", "[log_buffer]")
{
    log_buffer_init();
    esp_log_level_set(TEST_TAG, ESP_LOG_INFO);

    uint64_t start_pos = log_buffer_get_total_written();
    int start_notify = s_ws_notify_count;

    ESP_LOGI(TEST_TAG, "short_msg_marker_%d", 12345);

    uint64_t end_pos = log_buffer_get_total_written();
    TEST_ASSERT_TRUE(end_pos > start_pos);
    TEST_ASSERT_GREATER_THAN_INT(start_notify, s_ws_notify_count);

    char read_buf[256] = {0};
    uint64_t cursor = start_pos;
    size_t n = log_buffer_read_absolute(&cursor, read_buf, sizeof(read_buf) - 1);
    TEST_ASSERT_GREATER_THAN_UINT32(0, (uint32_t)n);
    TEST_ASSERT_NOT_NULL(strstr(read_buf, "short_msg_marker_12345"));

    /* Verify s_log_mutex was released and another task can acquire it without blocking */
    mutex_probe_ctx_t ctx = {
        .done_sem = xSemaphoreCreateBinary(),
        .total_written = 0,
    };
    TEST_ASSERT_NOT_NULL(ctx.done_sem);
    TEST_ASSERT_EQUAL(pdPASS, xTaskCreate(mutex_probe_task, "log_mutex_probe", 3072, &ctx, 5, NULL));
    TEST_ASSERT_EQUAL(pdTRUE, xSemaphoreTake(ctx.done_sem, pdMS_TO_TICKS(1000)));
    TEST_ASSERT_TRUE(ctx.total_written == end_pos);
    vSemaphoreDelete(ctx.done_sem);
}

TEST_CASE("log_buffer writes long line >= 2048 bytes without truncation", "[log_buffer]")
{
    log_buffer_init();
    esp_log_level_set(TEST_TAG, ESP_LOG_INFO);

    const size_t payload_len = 2400;
    char *long_msg = (char *)malloc(payload_len + 1);
    TEST_ASSERT_NOT_NULL(long_msg);

    memset(long_msg, 'A', payload_len);
    long_msg[payload_len] = '\0';
    memcpy(long_msg, "LONG_START_", 11);
    memcpy(long_msg + payload_len - 9, "_LONG_END", 9);

    uint64_t cursor = log_buffer_get_total_written();
    ESP_LOGI(TEST_TAG, "%s", long_msg);
    free(long_msg);

    uint64_t end_pos = log_buffer_get_total_written();
    TEST_ASSERT_TRUE((end_pos - cursor) >= payload_len);

    size_t cap = (size_t)(end_pos - cursor) + 64;
    char *read_buf = (char *)calloc(1, cap);
    TEST_ASSERT_NOT_NULL(read_buf);

    size_t total_read = 0;
    while (total_read < cap - 1) {
        size_t n = log_buffer_read_absolute(&cursor, read_buf + total_read, (cap - 1) - total_read);
        if (n == 0) {
            break;
        }
        total_read += n;
    }

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((uint32_t)payload_len, (uint32_t)total_read);
    TEST_ASSERT_NOT_NULL(strstr(read_buf, "LONG_START_"));
    TEST_ASSERT_NOT_NULL(strstr(read_buf, "_LONG_END"));
    free(read_buf);
}

TEST_CASE("log_buffer ring wrap-around preserves integrity", "[log_buffer]")
{
    log_buffer_init();
    esp_log_level_set(TEST_TAG, ESP_LOG_INFO);

    char filler[200];
    memset(filler, 'X', sizeof(filler) - 1);
    filler[sizeof(filler) - 1] = '\0';

    size_t iterations = (LOG_BUFFER_SIZE / 180) + 16;
    for (size_t i = 0; i < iterations; i++) {
        ESP_LOGI(TEST_TAG, "wrap_%u_%s", (unsigned)i, filler);
    }

    ESP_LOGI(TEST_TAG, "WRAP_TAIL_MARKER_%d", 999);

    uint64_t total = log_buffer_get_total_written();
    TEST_ASSERT_TRUE(total > LOG_BUFFER_SIZE);

    uint64_t cursor = 0;
    char line_buf[512];
    bool found_tail_marker = false;
    bool clamped_verified = false;

    while (1) {
        memset(line_buf, 0, sizeof(line_buf));
        size_t n = log_buffer_read_absolute(&cursor, line_buf, sizeof(line_buf) - 1);
        if (n == 0) {
            break;
        }
        if (!clamped_verified) {
            TEST_ASSERT_TRUE((cursor - n) >= (total - LOG_BUFFER_SIZE));
            clamped_verified = true;
        }
        if (strstr(line_buf, "WRAP_TAIL_MARKER_999") != NULL) {
            found_tail_marker = true;
        }
    }

    TEST_ASSERT_TRUE(clamped_verified);
    TEST_ASSERT_TRUE(found_tail_marker);
}
