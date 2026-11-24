#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
#include "esp_psram.h"
#endif
#include "i2c_bitaxe.h"
#include "unity.h"

static uint8_t *alloc_caller_buf(size_t size)
{
#if defined(CONFIG_SPIRAM) && CONFIG_SPIRAM
    if (esp_psram_is_initialized()) {
        return (uint8_t *)heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#endif
    return (uint8_t *)malloc(size);
}

TEST_CASE("i2c_bitaxe staging <= 64B uses internal DRAM static buffers and commits RX only on ESP_OK", "[i2c_bitaxe]")
{
    const size_t len = 16;
    uint8_t *tx_in = alloc_caller_buf(len);
    uint8_t *rx_out = alloc_caller_buf(len);
    TEST_ASSERT_NOT_NULL(tx_in);
    TEST_ASSERT_NOT_NULL(rx_out);

    for (size_t i = 0; i < len; i++) {
        tx_in[i] = (uint8_t)(0xA0 + i);
        rx_out[i] = 0x55;
    }

    i2c_staging_buffers_t staging;
    TEST_ASSERT_EQUAL(ESP_OK, i2c_bitaxe_staging_acquire(tx_in, len, len, &staging));
    TEST_ASSERT_NOT_NULL(staging.tx_buf);
    TEST_ASSERT_NOT_NULL(staging.rx_buf);
    TEST_ASSERT_NOT_EQUAL(tx_in, staging.tx_buf);
    TEST_ASSERT_NOT_EQUAL(rx_out, staging.rx_buf);
    TEST_ASSERT_TRUE(esp_ptr_internal(staging.tx_buf));
    TEST_ASSERT_TRUE(esp_ptr_internal(staging.rx_buf));
    TEST_ASSERT_FALSE(staging.tx_heap_allocated);
    TEST_ASSERT_FALSE(staging.rx_heap_allocated);
    TEST_ASSERT_TRUE(staging.holds_static_mutex);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(tx_in, staging.tx_buf, len);

    for (size_t i = 0; i < len; i++) {
        staging.rx_buf[i] = (uint8_t)(0x10 + i);
    }

    /* Failure must NOT overwrite caller_read_buf and must release mutex */
    i2c_bitaxe_staging_commit_and_release(&staging, rx_out, ESP_FAIL);
    for (size_t i = 0; i < len; i++) {
        TEST_ASSERT_EQUAL_UINT8(0x55, rx_out[i]);
    }

    /* Re-acquire (verifies mutex release) and commit with ESP_OK */
    uint8_t boundary_tx[I2C_STAGING_STATIC_BUF_SIZE];
    uint8_t boundary_rx[I2C_STAGING_STATIC_BUF_SIZE];
    memset(boundary_tx, 0x3C, sizeof(boundary_tx));
    memset(boundary_rx, 0x00, sizeof(boundary_rx));

    TEST_ASSERT_EQUAL(ESP_OK, i2c_bitaxe_staging_acquire(boundary_tx, sizeof(boundary_tx), sizeof(boundary_rx), &staging));
    TEST_ASSERT_TRUE(esp_ptr_internal(staging.tx_buf));
    TEST_ASSERT_TRUE(esp_ptr_internal(staging.rx_buf));
    TEST_ASSERT_FALSE(staging.tx_heap_allocated);
    TEST_ASSERT_FALSE(staging.rx_heap_allocated);
    TEST_ASSERT_TRUE(staging.holds_static_mutex);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(boundary_tx, staging.tx_buf, sizeof(boundary_tx));

    for (size_t i = 0; i < sizeof(boundary_rx); i++) {
        staging.rx_buf[i] = (uint8_t)(0x80 + i);
    }
    i2c_bitaxe_staging_commit_and_release(&staging, boundary_rx, ESP_OK);
    for (size_t i = 0; i < sizeof(boundary_rx); i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x80 + i), boundary_rx[i]);
    }

    free(tx_in);
    free(rx_out);
}

TEST_CASE("i2c_bitaxe staging > 64B allocates temporary internal DRAM and frees without leak", "[i2c_bitaxe]")
{
    const size_t tx_len = 128;
    const size_t rx_len = 96;
    uint8_t *large_tx = alloc_caller_buf(tx_len);
    uint8_t *large_rx = alloc_caller_buf(rx_len);
    TEST_ASSERT_NOT_NULL(large_tx);
    TEST_ASSERT_NOT_NULL(large_rx);

    for (size_t i = 0; i < tx_len; i++) {
        large_tx[i] = (uint8_t)(i ^ 0x5A);
    }
    memset(large_rx, 0, rx_len);

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    i2c_staging_buffers_t staging;
    TEST_ASSERT_EQUAL(ESP_OK, i2c_bitaxe_staging_acquire(large_tx, tx_len, rx_len, &staging));
    TEST_ASSERT_NOT_NULL(staging.tx_buf);
    TEST_ASSERT_NOT_NULL(staging.rx_buf);
    TEST_ASSERT_TRUE(esp_ptr_internal(staging.tx_buf));
    TEST_ASSERT_TRUE(esp_ptr_internal(staging.rx_buf));
    TEST_ASSERT_TRUE(staging.tx_heap_allocated);
    TEST_ASSERT_TRUE(staging.rx_heap_allocated);
    TEST_ASSERT_FALSE(staging.holds_static_mutex);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(large_tx, staging.tx_buf, tx_len);

    for (size_t i = 0; i < rx_len; i++) {
        staging.rx_buf[i] = (uint8_t)(0xF0 - i);
    }

    i2c_bitaxe_staging_commit_and_release(&staging, large_rx, ESP_OK);

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)free_before, (uint32_t)free_after);

    for (size_t i = 0; i < rx_len; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0xF0 - i), large_rx[i]);
    }

    free(large_tx);
    free(large_rx);
}

TEST_CASE("i2c_bitaxe staging returns ESP_ERR_NO_MEM when internal allocation fails", "[i2c_bitaxe]")
{
    uint8_t small_tx[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    const size_t impossible_len = (size_t)64 * 1024 * 1024;
    i2c_staging_buffers_t staging;

    /* Warm up static mutex before measuring internal heap */
    TEST_ASSERT_EQUAL(ESP_OK, i2c_bitaxe_staging_acquire(small_tx, sizeof(small_tx), 0, &staging));
    i2c_bitaxe_staging_commit_and_release(&staging, NULL, ESP_OK);

    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    /* Case 1: TX allocation > 64B fails */
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, i2c_bitaxe_staging_acquire(small_tx, impossible_len, 0, &staging));

    /* Case 2: Static TX acquires mutex, RX allocation > 64B fails -> must release mutex cleanly */
    TEST_ASSERT_EQUAL(ESP_ERR_NO_MEM, i2c_bitaxe_staging_acquire(small_tx, sizeof(small_tx), impossible_len, &staging));

    size_t free_after = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)free_before, (uint32_t)free_after);

    /* Mutex must still be available */
    TEST_ASSERT_EQUAL(ESP_OK, i2c_bitaxe_staging_acquire(small_tx, sizeof(small_tx), 4, &staging));
    TEST_ASSERT_TRUE(esp_ptr_internal(staging.tx_buf));
    TEST_ASSERT_TRUE(esp_ptr_internal(staging.rx_buf));
    i2c_bitaxe_staging_commit_and_release(&staging, NULL, ESP_FAIL);
}
