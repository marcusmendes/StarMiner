#include <string.h>
#include "device_config.h"
#include "nvs_config.h"
#include "global_state.h"
#include "esp_log.h"

#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static const char * TAG = "device_config";

static void apply_default_config(GlobalState * GLOBAL_STATE, const DeviceConfig * cfg, const char * override_board_version)
{
    GLOBAL_STATE->DEVICE_CONFIG = *cfg;
    if (override_board_version != NULL) {
        char * preserved_version = strdup(override_board_version);
        if (preserved_version != NULL) {
            GLOBAL_STATE->DEVICE_CONFIG.board_version = preserved_version;
        }
    }

    ESP_LOGI(TAG, "Device Model: %s", GLOBAL_STATE->DEVICE_CONFIG.family.name);
    ESP_LOGI(TAG, "Board Version: %s", GLOBAL_STATE->DEVICE_CONFIG.board_version);
    ESP_LOGI(TAG, "ASIC: %dx %s (%d cores)",
             GLOBAL_STATE->DEVICE_CONFIG.family.asic_count,
             GLOBAL_STATE->DEVICE_CONFIG.family.asic.name,
             GLOBAL_STATE->DEVICE_CONFIG.family.asic.core_count);
}

esp_err_t device_config_init(void * pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *) pvParameters;

    // TODO: Read board version from eFuse

    char * board_version = nvs_config_get_string(NVS_CONFIG_BOARD_VERSION);

    if (board_version != NULL) {
        for (int i = 0 ; i < ARRAY_SIZE(default_configs); i++) {
            if (strcmp(default_configs[i].board_version, board_version) == 0) {
                apply_default_config(GLOBAL_STATE, &default_configs[i], NULL);
                free(board_version);
                return ESP_OK;
            }
        }

        if (strncmp(board_version, "601a", 4) == 0) {
            for (int i = 0; i < ARRAY_SIZE(default_configs); i++) {
                if (strcmp(default_configs[i].board_version, "601a") == 0) {
                    ESP_LOGW(TAG, "Unknown 601a sub-revision '%s', falling back to 601a profile", board_version);
                    apply_default_config(GLOBAL_STATE, &default_configs[i], board_version);
                    free(board_version);
                    return ESP_OK;
                }
            }
        }
    }

    ESP_LOGI(TAG, "Custom Board Version: %s", board_version ? board_version : "(null)");

    GLOBAL_STATE->DEVICE_CONFIG.board_version = board_version ? strdup(board_version) : strdup("unknown");

    char * device_model = nvs_config_get_string(NVS_CONFIG_DEVICE_MODEL);

    bool family_matched = false;
    if (device_model != NULL) {
        for (int i = 0 ; i < ARRAY_SIZE(default_families); i++) {
            if (strcasecmp(default_families[i].name, device_model) == 0) {
                GLOBAL_STATE->DEVICE_CONFIG.family = default_families[i];

                ESP_LOGI(TAG, "Device Model: %s", GLOBAL_STATE->DEVICE_CONFIG.family.name);
                family_matched = true;
                break;
            }
        }

        if (!family_matched && strcasecmp(device_model, "LT-001") == 0) {
            GLOBAL_STATE->DEVICE_CONFIG.family = FAMILY_GAMMA_V2;
            ESP_LOGI(TAG, "Device Model (alias %s): %s", device_model, GLOBAL_STATE->DEVICE_CONFIG.family.name);
        }
    }

    char * asic_model = nvs_config_get_string(NVS_CONFIG_ASIC_MODEL);

    for (int i = 0 ; i < ARRAY_SIZE(default_asic_configs); i++) {
        if (strcasecmp(default_asic_configs[i].name, asic_model) == 0) {
            GLOBAL_STATE->DEVICE_CONFIG.family.asic = default_asic_configs[i];

            ESP_LOGI(TAG, "ASIC: %dx %s (%d cores)", GLOBAL_STATE->DEVICE_CONFIG.family.asic_count, GLOBAL_STATE->DEVICE_CONFIG.family.asic.name, GLOBAL_STATE->DEVICE_CONFIG.family.asic.core_count);

            break;
        }
    }

    GLOBAL_STATE->DEVICE_CONFIG.plug_sense = nvs_config_get_bool(NVS_CONFIG_PLUG_SENSE);
    GLOBAL_STATE->DEVICE_CONFIG.asic_enable = nvs_config_get_bool(NVS_CONFIG_ASIC_ENABLE);
    GLOBAL_STATE->DEVICE_CONFIG.EMC2101 = nvs_config_get_bool(NVS_CONFIG_EMC2101);
    GLOBAL_STATE->DEVICE_CONFIG.EMC2103 = nvs_config_get_bool(NVS_CONFIG_EMC2103);
    GLOBAL_STATE->DEVICE_CONFIG.EMC2302 = nvs_config_get_bool(NVS_CONFIG_EMC2302);
    GLOBAL_STATE->DEVICE_CONFIG.emc_internal_temp = nvs_config_get_bool(NVS_CONFIG_EMC_INTERNAL_TEMP);
    GLOBAL_STATE->DEVICE_CONFIG.emc_ideality_factor = nvs_config_get_u16(NVS_CONFIG_EMC_IDEALITY_FACTOR);
    GLOBAL_STATE->DEVICE_CONFIG.emc_beta_compensation = nvs_config_get_u16(NVS_CONFIG_EMC_BETA_COMPENSATION);
    GLOBAL_STATE->DEVICE_CONFIG.temp_offset = nvs_config_get_i32(NVS_CONFIG_TEMP_OFFSET);
    GLOBAL_STATE->DEVICE_CONFIG.DS4432U = nvs_config_get_bool(NVS_CONFIG_DS4432U);
    GLOBAL_STATE->DEVICE_CONFIG.INA260 = nvs_config_get_bool(NVS_CONFIG_INA260);
    GLOBAL_STATE->DEVICE_CONFIG.TPS546 = nvs_config_get_bool(NVS_CONFIG_TPS546);
    GLOBAL_STATE->DEVICE_CONFIG.TMP1075 = nvs_config_get_bool(NVS_CONFIG_TMP1075);

    // test values
    GLOBAL_STATE->DEVICE_CONFIG.power_consumption_target = nvs_config_get_u16(NVS_CONFIG_POWER_CONSUMPTION_TARGET);

    free(board_version);
    free(device_model);
    free(asic_model);

    return ESP_OK;
}
