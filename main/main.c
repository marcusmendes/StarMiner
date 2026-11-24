#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#endif
#include "cJSON.h"

#include "asic_result_task.h"
#include "create_jobs_task.h"
#include "hashrate_monitor_task.h"
#include "fan_controller_task.h"
#include "statistics_task.h"
#include "system.h"
#include "http_server.h"
#include "serial.h"
#include "protocol_coordinator.h"
#include "i2c_bitaxe.h"
#include "adc.h"
#include "nvs_config.h"
#include "self_test.h"
#include "asic.h"
#include "bap/bap.h"
#include "device_config.h"
#include "connect.h"
#include "asic_reset.h"
#include "asic_init.h"
#include "task_monitor.h"
#include "filesystem.h"
#include "input.h"
#include "log_buffer.h"

static GlobalState GLOBAL_STATE;

static const char * TAG = "bitaxe";

static inline UBaseType_t psram_stack_caps(void)
{
    return esp_psram_is_initialized() ? (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : MALLOC_CAP_DEFAULT;
}

static void heap_alloc_failed_hook(size_t requested_size, uint32_t caps, const char *function_name)
{
    if (caps & MALLOC_CAP_SPIRAM) {
        ESP_EARLY_LOGE(TAG, "%s failed to allocate %zu bytes from PSRAM", function_name, requested_size);
        abort();
    }
}

static void *cjson_malloc_psram(size_t size)
{
    if (esp_psram_is_initialized()) {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }
    return malloc(size);
}

static void cjson_free_psram(void *ptr)
{
    free(ptr);
}

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
static void log_stored_coredump_summary(void)
{
    if (esp_core_dump_image_check() != ESP_OK) {
        return;
    }

#if CONFIG_ESP_COREDUMP_DATA_FORMAT_ELF
    esp_core_dump_summary_t *summary = (esp_core_dump_summary_t *)calloc(1, sizeof(esp_core_dump_summary_t));
    if (summary == NULL) {
        ESP_LOGW(TAG, "Stored flash coredump detected, but failed to allocate summary buffer");
        return;
    }

    if (esp_core_dump_get_summary(summary) == ESP_OK) {
        char task_name[sizeof(summary->exc_task) + 1];
        memcpy(task_name, summary->exc_task, sizeof(summary->exc_task));
        task_name[sizeof(summary->exc_task)] = '\0';

        char bt_str[256];
        size_t pos = 0;
        bt_str[0] = '\0';
        uint32_t depth = summary->exc_bt_info.depth;
        if (depth > 16) {
            depth = 16;
        }
        for (uint32_t i = 0; i < depth && pos < sizeof(bt_str); i++) {
            int written = snprintf(bt_str + pos, sizeof(bt_str) - pos,
                                   "%s0x%08" PRIx32, (i == 0) ? "" : " ", summary->exc_bt_info.bt[i]);
            if (written < 0 || (size_t)written >= sizeof(bt_str) - pos) {
                break;
            }
            pos += (size_t)written;
        }

        esp_reset_reason_t reason = esp_reset_reason();
        if (reason == ESP_RST_PANIC) {
            ESP_LOGE(TAG,
                     "Stored panic coredump: task=%s pc=0x%08" PRIx32 " cause=0x%08" PRIx32
                     " vaddr=0x%08" PRIx32 " bt_depth=%" PRIu32 " corrupted=%d bt=[%s]",
                     task_name, summary->exc_pc, summary->ex_info.exc_cause,
                     summary->ex_info.exc_vaddr, summary->exc_bt_info.depth,
                     summary->exc_bt_info.corrupted ? 1 : 0, bt_str);
        } else {
            ESP_LOGI(TAG,
                     "Previous stored coredump (reset_reason=%d): task=%s pc=0x%08" PRIx32
                     " cause=0x%08" PRIx32 " vaddr=0x%08" PRIx32 " bt_depth=%" PRIu32
                     " corrupted=%d bt=[%s]",
                     (int)reason, task_name, summary->exc_pc, summary->ex_info.exc_cause,
                     summary->ex_info.exc_vaddr, summary->exc_bt_info.depth,
                     summary->exc_bt_info.corrupted ? 1 : 0, bt_str);
        }
    } else {
        ESP_LOGW(TAG, "Stored flash coredump present, but failed to parse ELF summary");
    }

    free(summary);
#else
    ESP_LOGI(TAG, "Stored flash coredump detected (reset_reason=%d)", (int)esp_reset_reason());
#endif
}
#endif

void app_main(void)
{
    ESP_ERROR_CHECK(heap_caps_register_failed_alloc_callback(heap_alloc_failed_hook));

    cJSON_Hooks hooks = {
        .malloc_fn = cjson_malloc_psram,
        .free_fn = cjson_free_psram
    };
    cJSON_InitHooks(&hooks);
    if (esp_psram_is_initialized()) {
        GLOBAL_STATE.psram_is_available = true;
        log_buffer_init();
    } else {
        ESP_LOGE(TAG, "No PSRAM available on ESP32 device!");
    }

#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    log_stored_coredump_summary();
#endif

    ESP_LOGI(TAG, "Welcome to the bitaxe - FOSS || GTFO!");

    if (xTaskCreatePinnedToCoreWithCaps(cpu_monitor_task, "cpu_monitor", 4096, (void *)&GLOBAL_STATE, 1, NULL, 0, psram_stack_caps()) != pdPASS) {
        ESP_LOGE(TAG, "Error creating cpu monitor task");
    }
#ifdef CONFIG_ENABLE_TASK_MONITOR
    if (xTaskCreatePinnedToCoreWithCaps(task_monitor_task, "task_monitor", 8192, NULL, 1, NULL, 0, psram_stack_caps()) != pdPASS) {
        ESP_LOGE(TAG, "Error creating task monitor task");
    }
#endif
  
    // Init I2C
    ESP_ERROR_CHECK(i2c_bitaxe_init());
    ESP_LOGI(TAG, "I2C initialized successfully");

    // Initialize RST pin to low early to minimize ASIC power consumption
    ESP_ERROR_CHECK(asic_hold_reset_low());
    ESP_LOGI(TAG, "RST pin initialized to low");

    // wait for I2C to init
    vTaskDelay(100 / portTICK_PERIOD_MS);

    // Init ADC
    ADC_init();

    // initialize the ESP32 NVS
    if (nvs_config_init() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init NVS");
        return;
    }

    // Ensure SSID is initialized before any screen/self-test uses it.
    GLOBAL_STATE.SYSTEM_MODULE.ssid = nvs_config_get_string(NVS_CONFIG_WIFI_SSID);
    if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
        ESP_LOGW(TAG, "No SSID configured in NVS, using empty string");
        GLOBAL_STATE.SYSTEM_MODULE.ssid = strdup("");
        if (GLOBAL_STATE.SYSTEM_MODULE.ssid == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory for SSID");
            return;
        }
    }

    if (device_config_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init device config");
        return;
    }

    if (self_test_init(&GLOBAL_STATE) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init self test");
        return;
    }

    SYSTEM_init_system(&GLOBAL_STATE);
    if (scoreboard_init(&GLOBAL_STATE.SYSTEM_MODULE.scoreboard) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to init scoreboard");
    }

    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        wifi_init(&GLOBAL_STATE);
    }

    esp_err_t system_init_ret = SYSTEM_init_peripherals(&GLOBAL_STATE);
    
    if (system_init_ret == ESP_OK) {
        if (xTaskCreatePinnedToCoreWithCaps(POWER_MANAGEMENT_task, "power management", 8192, (void *) &GLOBAL_STATE, 10, NULL, 0, psram_stack_caps()) != pdPASS) {
            ESP_LOGE(TAG, "Error creating power management task");
        }
        if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
            if (xTaskCreatePinnedToCoreWithCaps(FAN_CONTROLLER_task, "fan_controller", 8192, (void *) &GLOBAL_STATE, 5, NULL, 0, psram_stack_caps()) != pdPASS) {
                ESP_LOGE(TAG, "Error creating fan controller task");
            }
        }
    } else {
        ESP_LOGE(TAG, "Critical peripheral initialization failure (%s). Entering degraded mode.", esp_err_to_name(GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret));
    }
    
    if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        // start the API for AxeOS
        start_rest_server((void *) &GLOBAL_STATE);
    }

    // After mounting SPIFFS
    SYSTEM_init_versions(&GLOBAL_STATE);

    // Initialize BAP interface
    esp_err_t bap_ret = BAP_init(&GLOBAL_STATE);
    if (bap_ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize BAP interface: %d", bap_ret);
        // Continue anyway, as BAP is not critical for core functionality
    }

    while (!GLOBAL_STATE.SYSTEM_MODULE.is_connected) {
        vTaskDelay(100 / portTICK_PERIOD_MS);
    }

    queue_init(&GLOBAL_STATE.stratum_queue);

    if (system_init_ret == ESP_OK) {
        if (asic_initialize(&GLOBAL_STATE, ASIC_INIT_COLD_BOOT, 0) == 0) {
            if (!GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
                return;
            }

            self_test_show_message(&GLOBAL_STATE, GLOBAL_STATE.SYSTEM_MODULE.asic_status);
            system_init_ret = ESP_FAIL;
        } else {
            if (xTaskCreatePinnedToCoreWithCaps(create_jobs_task, "stratum miner", 8192, (void *) &GLOBAL_STATE, 10, NULL, 1, psram_stack_caps()) != pdPASS) {
                ESP_LOGE(TAG, "Error creating stratum miner task");
            }
            if (xTaskCreatePinnedToCoreWithCaps(ASIC_result_task, "asic result", 8192, (void *) &GLOBAL_STATE, 15, NULL, 1, psram_stack_caps()) != pdPASS) {
                ESP_LOGE(TAG, "Error creating asic result task");
            }

            if (xTaskCreatePinnedToCoreWithCaps(hashrate_monitor_task, "hashrate monitor", 8192, (void *) &GLOBAL_STATE, 5, NULL, 1, psram_stack_caps()) != pdPASS) {
                ESP_LOGE(TAG, "Error creating hashrate monitor task");
            }
            if (xTaskCreatePinnedToCoreWithCaps(statistics_task, "statistics", 8192, (void *) &GLOBAL_STATE, 3, NULL, 0, psram_stack_caps()) != pdPASS) {
                ESP_LOGE(TAG, "Error creating statistics task");
            }
        }
    }

    protocol_coordinator_init(&GLOBAL_STATE);
    if (xTaskCreatePinnedToCoreWithCaps(protocol_coordinator_task, "protocol coord", 3072, (void *) &GLOBAL_STATE, 5, NULL, 0, psram_stack_caps()) != pdPASS) {
        ESP_LOGE(TAG, "Error creating protocol coordinator task");
    }

    if (GLOBAL_STATE.SELF_TEST_MODULE.is_active) {
        GLOBAL_STATE.SELF_TEST_MODULE.system_init_ret = system_init_ret;
        if (xTaskCreatePinnedToCoreWithCaps(self_test_task, "self_test", 8192, (void *) &GLOBAL_STATE, 10, NULL, 0, psram_stack_caps()) != pdPASS) {
            ESP_LOGE(TAG, "Error creating self test task");
        }
    }
}
