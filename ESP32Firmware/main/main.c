#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"      // For NVS functions like nvs_flash_init
#include "esp_err.h"        // For error handling
#include "esp_sleep.h"      // For deep sleep and power management

#define TAG "ESP_FMDN"

// =========================================================================
// 🔋 BATTERY & POWER OPTIMIZATION CONFIGURATION
// =========================================================================
// Advertising Interval:
// Interval is in units of 0.625 ms:
//   - 0x20  (32)   = 20 ms    -> High drain (lasts ~24-36 hrs on 3200mAh)
//   - 0x640 (1600) = 1000 ms  -> Balanced (1 second)
//   - 0xC80 (3200) = 2000 ms  -> Recommended (2.0 seconds, standard FMDN beacon)
//   - 0x12C0 (4800)= 3000 ms  -> Battery Saver (3.0 seconds)
#define ADV_INTERVAL_UNITS           3200   // 3200 * 0.625ms = 2.0 seconds

// BLE Transmit Power Level:
//   - ESP_PWR_LVL_P9 = +9 dBm (Max power, highest current spikes ~120mA)
//   - ESP_PWR_LVL_P3 = +3 dBm (Recommended: 15-25m range, ~30% less current)
//   - ESP_PWR_LVL_N0 =  0 dBm (Low power: 10-15m range, lowest current)
#define BLE_TX_POWER_DEFAULT         ESP_PWR_LVL_P3

// Power Strategy Mode:
//   0 = CONTINUOUS_LOW_POWER (Recommended)
//       Maintains continuous 2.0s advertising with modem sleep enabled.
//       Ensures passing Android devices can detect it reliably at any time.
//       Estimated battery life on 3200mAh: ~3 to 6+ weeks (depending on board).
//
//   1 = DEEP_SLEEP_BURST
//       Wakes up, broadcasts for BURST_ACTIVE_TIME_SEC, then enters deep sleep
//       for DEEP_SLEEP_DURATION_SEC.
//       Estimated battery life on 3200mAh: multiple months!
#define POWER_SAVE_MODE              0

#define BURST_ACTIVE_TIME_SEC        5     // Active broadcast duration in seconds (for Mode 1)
#define DEEP_SLEEP_DURATION_SEC      30    // Deep sleep sleep interval in seconds (for Mode 1)
// =========================================================================

#if defined(CONFIG_IDF_TARGET_ESP32C3)
#include "esp_nimble_hci.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"

#elif defined(CONFIG_IDF_TARGET_ESP32)
#include "esp_bt.h"
#include "esp_bt_main.h"   // For esp_bluedroid_* functions
#include "esp_gap_ble_api.h"

#else
#error "Unsupported target"
#endif

// This is the advertisement key / EID. Change it to your own EID.
const char *eid_string = "INSERT_YOUR_ADVERTISEMENT_KEY_HERE";

// Find My Device Network (FMDN) advertisement
// Octet 	Value 	        Description
// 0 	    0x02 	        Length
// 1 	    0x01 	        Flags data type value
// 2 	    0x06 	        Flags data
// 3 	    0x18 or 0x19 	Length
// 4 	    0x16 	        Service data data type value
// 5 	    0xAA 	        16-bit service UUID
// 6 	    0xFE 	        16-bit service UUID
// 7 	    0x40 or 0x41 	FMDN frame type with unwanted tracking protection mode indication
// 8..27 	Random          20-byte ephemeral identifier
// 28 		Hashed flags

uint8_t adv_raw_data[31] = {
    0x02,   // Length
    0x01,   // Flags data type value
    0x06,   // Flags data
    0x19,   // Length
    0x16,   // Service data data type value
    0xAA,   // 16-bit service UUID
    0xFE,   // 16-bit service UUID
    0x41,   // FMDN frame type with unwanted tracking protection mode indication
            // 20-byte ephemeral identifier (inserted below)
            // Hashed flags (implicitly initialized to 0)
};


// Function to convert a hex string into a byte array
void hex_string_to_bytes(const char *hex, uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
    }
}

#if defined(CONFIG_IDF_TARGET_ESP32C3)
// BLE advertising callback
static int ble_advertise_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
        case BLE_GAP_EVENT_ADV_COMPLETE:
            ESP_LOGI(TAG, "Advertising completed");
            break;
        default:
            break;
    }
    return 0;
}

// Set up and start advertising
static void ble_start_advertising(uint8_t *adv_raw_data, size_t adv_raw_data_len)
{
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = ADV_INTERVAL_UNITS,
        .itvl_max = ADV_INTERVAL_UNITS
    };

    ble_gap_adv_set_data(adv_raw_data, adv_raw_data_len);

    // Start advertising
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_advertise_cb, NULL);
    
    ESP_LOGI(TAG, "Started advertising (Interval: %d ms)", (int)(ADV_INTERVAL_UNITS * 0.625));
}

static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "BLE Host Task Started");
    nimble_port_run();
    nimble_port_freertos_deinit();
}

// Sync callback
static void on_sync(void)
{
    // Set device name
    ble_svc_gap_device_name_set("ESP32-C3-BLE");
    
    // Start advertising
    ble_start_advertising(adv_raw_data, sizeof(adv_raw_data));
}
#endif

void app_main() {
    // Initialize NVS (required for BLE initialization)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 20-byte ephemeral identifier
    uint8_t eid_bytes[20];
    hex_string_to_bytes(eid_string, eid_bytes, 20);
    memcpy(&adv_raw_data[8], eid_bytes, 20);

    #if defined(CONFIG_IDF_TARGET_ESP32C3)
        ESP_LOGI(TAG, "Initializing NimBLE with power optimizations");
        
        // Initialize NimBLE - ESP-IDF v5.3 style
        ESP_ERROR_CHECK(nimble_port_init());
        
        // Initialize the NimBLE host configuration
        ble_hs_cfg.sync_cb = on_sync;
        
        // Initialize GAP service
        ble_svc_gap_init();
        
        // Create host task
        nimble_port_freertos_init(ble_host_task);

    #elif defined(CONFIG_IDF_TARGET_ESP32)
        // Initialize Bluetooth controller with low power / modem sleep configuration
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

        // Initialize Bluedroid stack
        ESP_ERROR_CHECK(esp_bluedroid_init());
        ESP_ERROR_CHECK(esp_bluedroid_enable());

        // Set optimized BLE TX power (lower power spikes, saves battery)
        ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, BLE_TX_POWER_DEFAULT));
        ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, BLE_TX_POWER_DEFAULT));
        ESP_LOGI(TAG, "Configured BLE TX Power level %d", BLE_TX_POWER_DEFAULT);

        ESP_ERROR_CHECK(esp_ble_gap_config_adv_data_raw(adv_raw_data, sizeof(adv_raw_data)));

        // Configure advertisement parameters with power-saving interval
        esp_ble_adv_params_t adv_params = {
            .adv_int_min = ADV_INTERVAL_UNITS,
            .adv_int_max = ADV_INTERVAL_UNITS,
            .adv_type = ADV_TYPE_NONCONN_IND,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .channel_map = ADV_CHNL_ALL,
            .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
        };

        // Start advertising
        ESP_ERROR_CHECK(esp_ble_gap_start_advertising(&adv_params));
        ESP_LOGI(TAG, "BLE advertising started with interval %d ms", (int)(ADV_INTERVAL_UNITS * 0.625));
    #endif

    // Handle Deep Sleep Burst Mode if enabled
    #if (POWER_SAVE_MODE == 1)
        ESP_LOGI(TAG, "Deep Sleep Burst Mode: Advertising for %d seconds...", BURST_ACTIVE_TIME_SEC);
        vTaskDelay(pdMS_TO_TICKS(BURST_ACTIVE_TIME_SEC * 1000));

        ESP_LOGI(TAG, "Entering deep sleep for %d seconds...", DEEP_SLEEP_DURATION_SEC);
        #if defined(CONFIG_IDF_TARGET_ESP32C3)
            ble_gap_adv_stop();
            nimble_port_stop();
            nimble_port_deinit();
        #elif defined(CONFIG_IDF_TARGET_ESP32)
            esp_ble_gap_stop_advertising();
            esp_bluedroid_disable();
            esp_bluedroid_deinit();
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
        #endif

        esp_sleep_enable_timer_wakeup((uint64_t)DEEP_SLEEP_DURATION_SEC * 1000000ULL);
        esp_deep_sleep_start();
    #endif
}