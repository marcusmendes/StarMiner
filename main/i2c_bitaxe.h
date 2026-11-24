#ifndef I2C_MASTER_H_
#define I2C_MASTER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/i2c_master.h"

#define I2C_BUS_SPEED_HZ 400000   /*!< I2C master clock frequency */
#define I2C_STAGING_STATIC_BUF_SIZE 64

typedef struct {
    uint8_t *tx_buf;
    uint8_t *rx_buf;
    size_t write_len;
    size_t read_len;
    bool tx_heap_allocated;
    bool rx_heap_allocated;
    bool holds_static_mutex;
} i2c_staging_buffers_t;

esp_err_t i2c_bitaxe_init(void);
esp_err_t i2c_bitaxe_add_device(uint8_t device_address, i2c_master_dev_handle_t * dev_handle, const char *device_tag);
esp_err_t i2c_bitaxe_get_master_bus_handle(i2c_master_bus_handle_t * dev_handle);

esp_err_t i2c_bitaxe_staging_acquire(const uint8_t *write_buf, size_t write_len,
                                     size_t read_len, i2c_staging_buffers_t *out);
void i2c_bitaxe_staging_commit_and_release(i2c_staging_buffers_t *staging,
                                           uint8_t *caller_read_buf,
                                           esp_err_t transfer_result);

esp_err_t i2c_bitaxe_register_read(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t * read_buf, size_t len);
esp_err_t i2c_bitaxe_register_write_addr(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr);
esp_err_t i2c_bitaxe_register_write_byte(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint8_t data);
esp_err_t i2c_bitaxe_register_write_bytes(i2c_master_dev_handle_t dev_handle, uint8_t * data, uint8_t len);
esp_err_t i2c_bitaxe_register_write_word(i2c_master_dev_handle_t dev_handle, uint8_t reg_addr, uint16_t data);

#endif /* I2C_MASTER_H_ */
