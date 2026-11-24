#include <string.h>
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "i2c_bitaxe.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifndef CONFIG_GPIO_I2C_SDA
#define CONFIG_GPIO_I2C_SDA 47
#endif
#ifndef CONFIG_GPIO_I2C_SCL
#define CONFIG_GPIO_I2C_SCL 48
#endif

#define GPIO_I2C_SDA CONFIG_GPIO_I2C_SDA
#define GPIO_I2C_SCL CONFIG_GPIO_I2C_SCL

#define I2C_MASTER_FREQ_HZ 100000

#define I2C_MASTER_NUM 0
#define I2C_MASTER_TIMEOUT_MS 500
#define I2C_RETRY_COUNT 3
#define I2C_RETRY_DELAY_MS 10

static i2c_master_bus_handle_t i2c_bus_handle;

static const char * TAG = "i2c_bitaxe";

DRAM_ATTR static uint8_t s_i2c_tx_staging[I2C_STAGING_STATIC_BUF_SIZE];
DRAM_ATTR static uint8_t s_i2c_rx_staging[I2C_STAGING_STATIC_BUF_SIZE];
static SemaphoreHandle_t s_i2c_staging_mutex = NULL;
static portMUX_TYPE s_i2c_staging_init_mux = portMUX_INITIALIZER_UNLOCKED;

typedef struct {
    i2c_master_dev_handle_t handle;
    uint16_t device_address;
    char device_tag[32];
} i2c_dev_map_entry_t;

#define MAX_DEVICES 10 // Adjust as needed
static i2c_dev_map_entry_t i2c_device_map[MAX_DEVICES];
static int i2c_device_count = 0;

static bool ensure_i2c_staging_mutex(void)
{
    if (s_i2c_staging_mutex != NULL) {
        return true;
    }

    SemaphoreHandle_t created = xSemaphoreCreateMutex();
    if (created == NULL) {
        return false;
    }

    portENTER_CRITICAL(&s_i2c_staging_init_mux);
    if (s_i2c_staging_mutex == NULL) {
        s_i2c_staging_mutex = created;
        created = NULL;
    }
    portEXIT_CRITICAL(&s_i2c_staging_init_mux);

    if (created != NULL) {
        vSemaphoreDelete(created);
    }

    return s_i2c_staging_mutex != NULL;
}

esp_err_t i2c_bitaxe_staging_acquire(const uint8_t *write_buf, size_t write_len,
                                     size_t read_len, i2c_staging_buffers_t *out)
{
    if (out == NULL || (write_len > 0 && write_buf == NULL)) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(out, 0, sizeof(*out));
    out->write_len = write_len;
    out->read_len = read_len;

    bool use_static_tx = (write_len > 0 && write_len <= I2C_STAGING_STATIC_BUF_SIZE);
    bool use_static_rx = (read_len > 0 && read_len <= I2C_STAGING_STATIC_BUF_SIZE);

    if (use_static_tx || use_static_rx) {
        if (!ensure_i2c_staging_mutex()) {
            return ESP_ERR_NO_MEM;
        }
        xSemaphoreTake(s_i2c_staging_mutex, portMAX_DELAY);
        out->holds_static_mutex = true;
    }

    if (write_len > 0) {
        if (use_static_tx) {
            out->tx_buf = s_i2c_tx_staging;
        } else {
            out->tx_buf = (uint8_t *)heap_caps_malloc(write_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (out->tx_buf == NULL) {
                i2c_bitaxe_staging_commit_and_release(out, NULL, ESP_ERR_NO_MEM);
                return ESP_ERR_NO_MEM;
            }
            out->tx_heap_allocated = true;
        }
        memcpy(out->tx_buf, write_buf, write_len);
    }

    if (read_len > 0) {
        if (use_static_rx) {
            out->rx_buf = s_i2c_rx_staging;
        } else {
            out->rx_buf = (uint8_t *)heap_caps_malloc(read_len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            if (out->rx_buf == NULL) {
                i2c_bitaxe_staging_commit_and_release(out, NULL, ESP_ERR_NO_MEM);
                return ESP_ERR_NO_MEM;
            }
            out->rx_heap_allocated = true;
        }
        memset(out->rx_buf, 0, read_len);
    }

    return ESP_OK;
}

void i2c_bitaxe_staging_commit_and_release(i2c_staging_buffers_t *staging,
                                           uint8_t *caller_read_buf,
                                           esp_err_t transfer_result)
{
    if (staging == NULL) {
        return;
    }

    if (transfer_result == ESP_OK && caller_read_buf != NULL &&
        staging->rx_buf != NULL && staging->read_len > 0) {
        memcpy(caller_read_buf, staging->rx_buf, staging->read_len);
    }

    if (staging->tx_heap_allocated && staging->tx_buf != NULL) {
        heap_caps_free(staging->tx_buf);
    }
    if (staging->rx_heap_allocated && staging->rx_buf != NULL) {
        heap_caps_free(staging->rx_buf);
    }
    if (staging->holds_static_mutex && s_i2c_staging_mutex != NULL) {
        xSemaphoreGive(s_i2c_staging_mutex);
    }

    memset(staging, 0, sizeof(*staging));
}

static const i2c_dev_map_entry_t *find_device_map_entry(i2c_master_dev_handle_t dev_handle)
{
    for (int i = 0; i < i2c_device_count; i++) {
        if (i2c_device_map[i].handle == dev_handle) {
            return &i2c_device_map[i];
        }
    }
    return NULL;
}

static esp_err_t i2c_transfer_with_retries(i2c_master_dev_handle_t dev_handle, 
                                           const uint8_t *write_buf, size_t write_len, 
                                           uint8_t *read_buf, size_t read_len)
{
    size_t effective_read_len = (read_buf != NULL && read_len > 0) ? read_len : 0;
    i2c_staging_buffers_t staging;
    esp_err_t err = i2c_bitaxe_staging_acquire(write_buf, write_len, effective_read_len, &staging);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to prepare internal DRAM staging buffers (%s)", esp_err_to_name(err));
        return err;
    }

    err = ESP_FAIL;

    for (int i = 0; i < I2C_RETRY_COUNT; i++) {
        if (effective_read_len > 0) {
            err = i2c_master_transmit_receive(dev_handle, staging.tx_buf, staging.write_len,
                                              staging.rx_buf, staging.read_len, I2C_MASTER_TIMEOUT_MS);
        } else {
            err = i2c_master_transmit(dev_handle, staging.tx_buf, staging.write_len, I2C_MASTER_TIMEOUT_MS);
        }

        if (err == ESP_OK) {
            i2c_bitaxe_staging_commit_and_release(&staging, read_buf, ESP_OK);
            return ESP_OK;
        }

        if (i + 1 < I2C_RETRY_COUNT) {
            const i2c_dev_map_entry_t *entry = find_device_map_entry(dev_handle);
            ESP_LOGW(TAG, "I2C transfer failed for [%s] (0x%02x) on attempt %d/%d (%s) - resetting bus",
                     entry ? entry->device_tag : "unknown",
                     (unsigned int)(entry ? entry->device_address : 0),
                     i + 1, I2C_RETRY_COUNT, esp_err_to_name(err));
            esp_err_t reset_err = i2c_master_bus_reset(i2c_bus_handle);
            if (reset_err != ESP_OK) {
                ESP_LOGW(TAG, "i2c_master_bus_reset failed: %s", esp_err_to_name(reset_err));
            }
            vTaskDelay(pdMS_TO_TICKS(I2C_RETRY_DELAY_MS));
        }
    }

    i2c_bitaxe_staging_commit_and_release(&staging, read_buf, err);

    const i2c_dev_map_entry_t *entry = find_device_map_entry(dev_handle);
    if (entry != NULL) {
        ESP_LOGE(TAG, "FATAL: [%s] (0x%02x) failed all %d retries.", entry->device_tag, entry->device_address, I2C_RETRY_COUNT);
        return err;
    }
    
    ESP_LOGE(TAG, "Unknown device");
    return err;
}

/**
 * @brief i2c master initialization
 */
esp_err_t i2c_bitaxe_init(void)
{
    if (!ensure_i2c_staging_mutex()) {
        return ESP_ERR_NO_MEM;
    }

    i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = I2C_MASTER_NUM,
        .scl_io_num = GPIO_I2C_SCL,
        .sda_io_num = GPIO_I2C_SDA,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    return i2c_new_master_bus(&i2c_bus_config, &i2c_bus_handle);
}

/**
 * @brief Add a new I2C Device
 * @param device_address The I2C device address
 * @param dev_handle The I2C device handle
 */
esp_err_t i2c_bitaxe_add_device(uint8_t device_address, i2c_master_dev_handle_t * dev_handle, const char *device_tag)
{
    if (i2c_device_count >= MAX_DEVICES) {
        ESP_LOGE(TAG, "Device map full, cannot add more devices");
        return ESP_FAIL;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = device_address,
        .scl_speed_hz = I2C_BUS_SPEED_HZ,
    };

    ESP_RETURN_ON_ERROR(i2c_master_bus_add_device(i2c_bus_handle, &dev_cfg, dev_handle), TAG, "Device 0x%02x", device_address);

    i2c_device_map[i2c_device_count].handle = *dev_handle;
    i2c_device_map[i2c_device_count].device_address = device_address;
    strncpy(i2c_device_map[i2c_device_count].device_tag, device_tag, sizeof(i2c_device_map[i2c_device_count].device_tag) - 1);
    i2c_device_map[i2c_device_count].device_tag[sizeof(i2c_device_map[i2c_device_count].device_tag) - 1] = '\0';
    i2c_device_count++;

    return ESP_OK;
}

esp_err_t i2c_bitaxe_get_master_bus_handle(i2c_master_bus_handle_t * dev_handle)
{
    *dev_handle = i2c_bus_handle;
    return ESP_OK;
}

/**
 * @brief Read a sequence of I2C bytes
 * @param dev_handle The I2C device handle
 * @param reg_addr The register address to read from
 * @param read_buf The buffer to store the read data
 * @param len The number of bytes to read
 */
esp_err_t i2c_bitaxe_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t * read_buf, size_t len)
{
    return i2c_transfer_with_retries(dev_handle, &reg_addr, 1, read_buf, len);
}

/**
 * @brief Just write a register address to the I2C device
 * 
 * @param dev_handle 
 * @param reg_addr 
 * @return esp_err_t 
 */
esp_err_t i2c_bitaxe_register_write_addr(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr)
{
    return i2c_transfer_with_retries(dev_handle, &reg_addr, 1, NULL, 0);
}

/**
 * @brief Write a byte to a I2C register
 * @param dev_handle The I2C device handle
 * @param reg_addr The register address to write to
 * @param data The data to write
 */
esp_err_t i2c_bitaxe_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data)
{
    uint8_t write_buf[2] = {reg_addr, data};
    return i2c_transfer_with_retries(dev_handle, write_buf, 2, NULL, 0);
}

/**
 * @brief Write a bytes to a I2C register
 * @param dev_handle The I2C device handle
 * @param data The data to write
 * @param len The number of bytes to write
 */
esp_err_t i2c_bitaxe_register_write_bytes(i2c_master_dev_handle_t dev_handle, uint8_t * data, uint8_t len)
{
    return i2c_transfer_with_retries(dev_handle, data, len, NULL, 0);
}

/**
 * @brief Write a word to a I2C register
 * @param dev_handle The I2C device handle
 * @param reg_addr The register address to write to
 * @param data The data to write
 */
esp_err_t i2c_bitaxe_register_write_word(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint16_t data)
{
    uint8_t write_buf[3] = {reg_addr, (uint8_t)(data & 0x00FF), (uint8_t)((data & 0xFF00) >> 8)};
    return i2c_transfer_with_retries(dev_handle, write_buf, 3, NULL, 0);
}
