#include <string.h>
#include <inttypes.h>
#include <esp_heap_caps.h>
#include <math.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "system.h"
#include "asic_common.h"
#include "asic.h"
#include "utils.h"

#define EPSILON 0.0001f

#define HASHRATE_UNIT 0x100000uLL // Hashrate register unit (2^24 hashes)

#define POLL_RATE 1000
#define HASHRATE_1M_SIZE (60000 / POLL_RATE)  // 12
#define HASHRATE_10M_SIZE 10
#define HASHRATE_1H_SIZE 6
#define DIV_10M (HASHRATE_1M_SIZE)
#define DIV_1H (HASHRATE_10M_SIZE * DIV_10M)

#define WATCHDOG_GRACE_PERIOD_US      (30LL * 1000000LL)
#define WATCHDOG_ZERO_HASHRATE_US     (60LL * 1000000LL)
#define WATCHDOG_DOMAIN_FREEZE_US     (45LL * 1000000LL)

static unsigned long poll_count = 0;
static float hashrate_1m[HASHRATE_1M_SIZE];
static float hashrate_10m_prev;
static float hashrate_10m[HASHRATE_10M_SIZE];
static float hashrate_1h_prev;
static float hashrate_1h[HASHRATE_1H_SIZE];

static const char *TAG = "hashrate_monitor";

static float sum_hashrates(measurement_t * measurement, int asic_count)
{
    if (asic_count == 1) return measurement[0].hashrate;

    float total = 0;
    for (int asic_nr = 0; asic_nr < asic_count; asic_nr++) {
        total += measurement[asic_nr].hashrate;
    }
    return total;
}

/**
 * @brief Detect if any ASIC hash domain is frozen (< 10% of the average of the other domains).
 * Must be called while holding HASHRATE_MONITOR_MODULE->lock.
 */
static bool has_frozen_domain(const HashrateMonitorModule * monitor, int asic_count, int hash_domains,
                              int * out_asic, int * out_domain, float * out_domain_hr, float * out_other_avg)
{
    if (hash_domains <= 1) {
        return false;
    }

    for (int asic_nr = 0; asic_nr < asic_count; asic_nr++) {
        float sum = 0.0f;
        for (int d = 0; d < hash_domains; d++) {
            sum += monitor->domain_measurements[asic_nr][d].hashrate;
        }

        for (int d = 0; d < hash_domains; d++) {
            float d_hr = monitor->domain_measurements[asic_nr][d].hashrate;
            float other_avg = (sum - d_hr) / (float)(hash_domains - 1);
            if (other_avg > EPSILON && d_hr < (0.10f * other_avg)) {
                if (out_asic) *out_asic = asic_nr;
                if (out_domain) *out_domain = d;
                if (out_domain_hr) *out_domain_hr = d_hr;
                if (out_other_avg) *out_other_avg = other_avg;
                return true;
            }
        }
    }

    return false;
}

void hashrate_monitor_reset_measurements(void *pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *)pvParameters;    
    HashrateMonitorModule * HASHRATE_MONITOR_MODULE = &GLOBAL_STATE->HASHRATE_MONITOR_MODULE;

    if (!HASHRATE_MONITOR_MODULE->is_initialized) {
        return;
    }

    int asic_count = GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;
    int hash_domains = GLOBAL_STATE->DEVICE_CONFIG.family.asic.hash_domains;

    pthread_mutex_lock(&HASHRATE_MONITOR_MODULE->lock);
    memset(HASHRATE_MONITOR_MODULE->total_measurement, 0, asic_count * sizeof(measurement_t));
    memset(HASHRATE_MONITOR_MODULE->domain_measurements[0], 0, asic_count * hash_domains * sizeof(measurement_t));
    memset(HASHRATE_MONITOR_MODULE->error_measurement, 0, asic_count * sizeof(measurement_t));
    pthread_mutex_unlock(&HASHRATE_MONITOR_MODULE->lock);
}

void update_hashrate(measurement_t * measurement, uint32_t value)
{
    uint8_t flag_long = (value & 0x80000000) >> 31;
    uint32_t hashrate_value = value & 0x7FFFFFFF;    

    if (hashrate_value != 0x007FFFFF && !flag_long) {
        float hashrate = hashrate_value * (float)HASHRATE_UNIT; // Make sure it stays in float
        measurement->hashrate =  hashrate / 1e9f; // Convert to Gh/s
    }
}

void update_hash_counter(measurement_t * measurement, uint32_t value, uint64_t time_us)
{
    uint64_t previous_time_us = measurement->time_us;
    if (previous_time_us != 0) {
        uint64_t duration_us = time_us - previous_time_us;
        if (duration_us < 1000000) {
            // Ignore updates that are too close (e.g. rapid bursts) to avoid huge hashrate spikes
            return;
        }
        uint32_t counter = value - measurement->value; // Compute counter difference, handling uint32_t wraparound
        measurement->hashrate = hashCounterToGhs(duration_us, counter);
    }

    measurement->value = value;
    measurement->time_us = time_us;
}

static void init_averages()
{
    float nan_val = nanf("");
    for (int i = 0; i < HASHRATE_1M_SIZE; i++) hashrate_1m[i] = nan_val;
    for (int i = 0; i < HASHRATE_10M_SIZE; i++) hashrate_10m[i] = nan_val;
    for (int i = 0; i < HASHRATE_1H_SIZE; i++) hashrate_1h[i] = nan_val;
}

static float calculate_avg_nan_safe(const float arr[], int size) {
    float sum = 0.0f;
    int count = 0;
    for (int i = 0; i < size; i++) {
        if (!isnanf(arr[i])) {
            sum += arr[i];
            count++;
        }
    }
    return (count > 0) ? (sum / count) : 0.0f;
}

static void update_hashrate_averages(SystemModule * SYSTEM_MODULE)
{
    hashrate_1m[poll_count % HASHRATE_1M_SIZE] = SYSTEM_MODULE->current_hashrate;
    SYSTEM_MODULE->hashrate_1m = calculate_avg_nan_safe(hashrate_1m, HASHRATE_1M_SIZE);

    int hashrate_10m_blend = poll_count % HASHRATE_1M_SIZE;
    if (hashrate_10m_blend == 0) {
        hashrate_10m_prev = hashrate_10m[(poll_count / DIV_10M) % HASHRATE_10M_SIZE];
    }
    float hashrate_1m_value = SYSTEM_MODULE->hashrate_1m;
    if (!isnanf(hashrate_10m_prev)) {
        float f = (hashrate_10m_blend + 1.0f) / (float)HASHRATE_1M_SIZE;
        hashrate_1m_value = f * hashrate_1m_value + (1.0f - f) * hashrate_10m_prev;
    }

    hashrate_10m[(poll_count / DIV_10M) % HASHRATE_10M_SIZE] = hashrate_1m_value;
    SYSTEM_MODULE->hashrate_10m = calculate_avg_nan_safe(hashrate_10m, HASHRATE_10M_SIZE);

    int hashrate_1h_blend = poll_count % DIV_1H;
    if (hashrate_1h_blend == 0) {
        hashrate_1h_prev = hashrate_1h[(poll_count / DIV_1H) % HASHRATE_1H_SIZE];
    }
    float hashrate_10m_value = SYSTEM_MODULE->hashrate_10m;
    if (!isnanf(hashrate_1h_prev)) {
        float f = (hashrate_1h_blend + 1.0f) / (float)DIV_1H;
        hashrate_10m_value = f * hashrate_10m_value + (1.0f - f) * hashrate_1h_prev;
    }

    hashrate_1h[(poll_count / DIV_1H) % HASHRATE_1H_SIZE] = hashrate_10m_value;
    SYSTEM_MODULE->hashrate_1h = calculate_avg_nan_safe(hashrate_1h, HASHRATE_1H_SIZE);

    poll_count++;
}

void hashrate_monitor_task(void *pvParameters)
{
    GlobalState * GLOBAL_STATE = (GlobalState *)pvParameters;
    HashrateMonitorModule * HASHRATE_MONITOR_MODULE = &GLOBAL_STATE->HASHRATE_MONITOR_MODULE;
    SystemModule * SYSTEM_MODULE = &GLOBAL_STATE->SYSTEM_MODULE;

    int asic_count = GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;
    int hash_domains = GLOBAL_STATE->DEVICE_CONFIG.family.asic.hash_domains;

    HASHRATE_MONITOR_MODULE->total_measurement = heap_caps_malloc(asic_count * sizeof(measurement_t), MALLOC_CAP_SPIRAM);
    measurement_t* data = heap_caps_malloc(asic_count * hash_domains * sizeof(measurement_t), MALLOC_CAP_SPIRAM);
    HASHRATE_MONITOR_MODULE->domain_measurements = heap_caps_malloc(asic_count * sizeof(measurement_t*), MALLOC_CAP_SPIRAM);
    for (size_t asic_nr = 0; asic_nr < asic_count; asic_nr++) {
        HASHRATE_MONITOR_MODULE->domain_measurements[asic_nr] = data + (asic_nr * hash_domains);
    }
    HASHRATE_MONITOR_MODULE->error_measurement = heap_caps_malloc(asic_count * sizeof(measurement_t), MALLOC_CAP_SPIRAM);

    pthread_mutex_init(&HASHRATE_MONITOR_MODULE->lock, NULL);
    HASHRATE_MONITOR_MODULE->is_initialized = true;

    hashrate_monitor_reset_measurements(GLOBAL_STATE);

    init_averages();

    bool was_asic_initialized = false;
    bool was_watchdog_armed = false;
    int64_t asic_init_seen_us = 0;
    int64_t last_watchdog_check_us = 0;
    int64_t zero_hashrate_accum_us = 0;
    int64_t domain_freeze_accum_us = 0;
    TickType_t taskWakeTime = xTaskGetTickCount();
    while (1) {
        bool is_asic_initialized = GLOBAL_STATE->ASIC_initalized;
        int64_t now_us = esp_timer_get_time();

        if (!was_asic_initialized && is_asic_initialized) {
            asic_init_seen_us = now_us;
        }

        if (was_asic_initialized && !is_asic_initialized) {
            // ASIC just stopped (pause or overheat): clear measurements so that
            // time_us resets to 0. This prevents update_hash_counter from computing
            // a huge uint32_t wraparound diff (counter resets to 0 on ASIC reset)
            // which would cause a hashrate spike when resuming.
            hashrate_monitor_reset_measurements(GLOBAL_STATE);
        }
        was_asic_initialized = is_asic_initialized;

        bool any_domain_frozen = false;
        int frozen_asic = 0;
        int frozen_domain = 0;
        float frozen_hr = 0.0f;
        float frozen_other_avg = 0.0f;

        if (is_asic_initialized) {
            ASIC_read_registers(GLOBAL_STATE);
            vTaskDelay(100 / portTICK_PERIOD_MS);

            pthread_mutex_lock(&HASHRATE_MONITOR_MODULE->lock);
            float current_hashrate = sum_hashrates(HASHRATE_MONITOR_MODULE->total_measurement, asic_count);
            float error_hashrate = sum_hashrates(HASHRATE_MONITOR_MODULE->error_measurement, asic_count);
            any_domain_frozen = has_frozen_domain(HASHRATE_MONITOR_MODULE, asic_count, hash_domains,
                                                  &frozen_asic, &frozen_domain, &frozen_hr, &frozen_other_avg);
            pthread_mutex_unlock(&HASHRATE_MONITOR_MODULE->lock);

            SYSTEM_MODULE->current_hashrate = current_hashrate;
            SYSTEM_MODULE->error_percentage = current_hashrate > 0 ? error_hashrate / current_hashrate * 100.f : 0;

            if (current_hashrate > 0.0f) update_hashrate_averages(SYSTEM_MODULE);
        } else {
            SYSTEM_MODULE->current_hashrate = 0;
        }

        now_us = esp_timer_get_time();
        int64_t last_freq_change_us = GLOBAL_STATE->POWER_MANAGEMENT_MODULE.last_freq_change_us;
        bool grace_elapsed = (last_freq_change_us > 0)
                          && (asic_init_seen_us > 0)
                          && ((now_us - last_freq_change_us) >= WATCHDOG_GRACE_PERIOD_US)
                          && ((now_us - asic_init_seen_us) >= WATCHDOG_GRACE_PERIOD_US);

        bool watchdog_armed = is_asic_initialized
                           && (SYSTEM_MODULE->work_received > 0)
                           && !SYSTEM_MODULE->pools_unavailable
                           && !SYSTEM_MODULE->mining_paused
                           && (SYSTEM_MODULE->overheat_mode == 0)
                           && (SYSTEM_MODULE->power_fault == 0)
                           && !SYSTEM_MODULE->hardware_fault
                           && !GLOBAL_STATE->SELF_TEST_MODULE.is_active
                           && !GLOBAL_STATE->POWER_MANAGEMENT_MODULE.asic_watchdog_trigger
                           && grace_elapsed;

        if (!watchdog_armed) {
            zero_hashrate_accum_us = 0;
            domain_freeze_accum_us = 0;
            was_watchdog_armed = false;
        } else {
            int64_t dt_us = was_watchdog_armed ? (now_us - last_watchdog_check_us) : 0;
            last_watchdog_check_us = now_us;
            was_watchdog_armed = true;

            if (SYSTEM_MODULE->current_hashrate == 0.0f) {
                zero_hashrate_accum_us += dt_us;
            } else {
                zero_hashrate_accum_us = 0;
            }

            if (any_domain_frozen) {
                domain_freeze_accum_us += dt_us;
            } else {
                domain_freeze_accum_us = 0;
            }

            if (zero_hashrate_accum_us >= WATCHDOG_ZERO_HASHRATE_US) {
                ESP_LOGW(TAG, "ASIC Watchdog triggered: total hashrate at 0.0 GH/s for %lld s",
                         (long long)(zero_hashrate_accum_us / 1000000LL));
                GLOBAL_STATE->POWER_MANAGEMENT_MODULE.asic_watchdog_trigger = true;
                zero_hashrate_accum_us = 0;
                domain_freeze_accum_us = 0;
                was_watchdog_armed = false;
            } else if (domain_freeze_accum_us >= WATCHDOG_DOMAIN_FREEZE_US) {
                ESP_LOGW(TAG, "ASIC Watchdog triggered: ASIC %d domain %d frozen (%.2f GH/s < 10%% of %.2f GH/s avg) for %lld s",
                         frozen_asic, frozen_domain, frozen_hr, frozen_other_avg,
                         (long long)(domain_freeze_accum_us / 1000000LL));
                GLOBAL_STATE->POWER_MANAGEMENT_MODULE.asic_watchdog_trigger = true;
                zero_hashrate_accum_us = 0;
                domain_freeze_accum_us = 0;
                was_watchdog_armed = false;
            }
        }

        vTaskDelayUntil(&taskWakeTime, POLL_RATE / portTICK_PERIOD_MS);
    }
}

void hashrate_monitor_register_read(void *pvParameters, register_type_t register_type, uint8_t asic_nr, uint32_t value, uint64_t timestamp_us)
{
    GlobalState * GLOBAL_STATE = (GlobalState *)pvParameters;
    HashrateMonitorModule * HASHRATE_MONITOR_MODULE = &GLOBAL_STATE->HASHRATE_MONITOR_MODULE;

    int asic_count = GLOBAL_STATE->DEVICE_CONFIG.family.asic_count;

    if (asic_nr >= asic_count) {
        ESP_LOGE(TAG, "Asic nr out of bounds [%d]", asic_nr);
        return;
    }

    pthread_mutex_lock(&HASHRATE_MONITOR_MODULE->lock);

    switch(register_type) {
        case REGISTER_HASHRATE:
            update_hashrate(&HASHRATE_MONITOR_MODULE->total_measurement[asic_nr], value);
            update_hashrate(&HASHRATE_MONITOR_MODULE->domain_measurements[asic_nr][0], value);
            break;
        case REGISTER_TOTAL_COUNT:
            update_hash_counter(&HASHRATE_MONITOR_MODULE->total_measurement[asic_nr], value, timestamp_us);
            break;
        case REGISTER_DOMAIN_0_COUNT:
            update_hash_counter(&HASHRATE_MONITOR_MODULE->domain_measurements[asic_nr][0], value, timestamp_us);
            break;
        case REGISTER_DOMAIN_1_COUNT:
            update_hash_counter(&HASHRATE_MONITOR_MODULE->domain_measurements[asic_nr][1], value, timestamp_us);
            break;
        case REGISTER_DOMAIN_2_COUNT:
            update_hash_counter(&HASHRATE_MONITOR_MODULE->domain_measurements[asic_nr][2], value, timestamp_us);
            break;
        case REGISTER_DOMAIN_3_COUNT:
            update_hash_counter(&HASHRATE_MONITOR_MODULE->domain_measurements[asic_nr][3], value, timestamp_us);
            break;
        case REGISTER_ERROR_COUNT:
            update_hash_counter(&HASHRATE_MONITOR_MODULE->error_measurement[asic_nr], value, timestamp_us);
            break;
        case REGISTER_PLL_PARAM:
            ESP_LOGD(TAG, "PLL param read asic %d: 0x%08" PRIX32, asic_nr, value);
            break;
        case REGISTER_INVALID:
            ESP_LOGE(TAG, "Invalid register type");
            break;
    }

    pthread_mutex_unlock(&HASHRATE_MONITOR_MODULE->lock);
}

/*
    // From NerdAxe codebase, temparature conversion?
    if (asic_result.data & 0x80000000) {
        float ftemp = (float) (asic_result.data & 0x0000ffff) * 0.171342f - 299.5144f;
        ESP_LOGI(TAG, "asic %d temp: %.3f", (int) asic_result.asic_nr, ftemp);
        board->setChipTemp(asic_result.asic_nr, ftemp);
    }
    break;
*/
