#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"     // For esp_reset_reason()
#include "nvs_flash.h"      // For NVS functions
#include "nvs.h"            // For NVS storage
#include "esp_err.h"        // For error handling
#include "esp_sleep.h"      // For deep sleep
#include "driver/gpio.h"    // For status LED control

#define TAG "ESP_FMDN"

// =========================================================================
// 🔋 HARDWARE & POWER OPTIMIZATION CONFIGURATION
// =========================================================================

// Advertising Interval:
// In units of 0.625 ms:
//   1600 * 0.625ms = 1000 ms (1.0 second: fast Radar Arrow updates + ~3-4 weeks battery life)
#define ADV_INTERVAL_UNITS           1600

// BLE Transmit Power:
//   ESP_PWR_LVL_P3 (+3 dBm) gives 15-25m range with ~30% lower current spikes.
#define BLE_TX_POWER_DEFAULT         ESP_PWR_LVL_P3

// Settings for Mode 1 (Deep Sleep Burst Mode):
#define BURST_ACTIVE_TIME_SEC        5     // Active broadcast duration in seconds
#define DEEP_SLEEP_DURATION_SEC      30    // Deep sleep interval in seconds

// Status LED Pin Configuration:
//   ESP32-CAM onboard red LED is on GPIO 33 (Active LOW: 0 = ON, 1 = OFF).
//   For standard ESP32 DevKit V1, change STATUS_LED_PIN to 2 and LED_ACTIVE_LEVEL to 1.
#if defined(CONFIG_IDF_TARGET_ESP32)
#define STATUS_LED_PIN               33    // GPIO 33 on ESP32-CAM
#define LED_ACTIVE_LEVEL             0     // 0 = Active LOW (ESP32-CAM)
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
#define STATUS_LED_PIN               8     // GPIO 8 on typical ESP32-C3
#define LED_ACTIVE_LEVEL             0
#endif

// Double-Tap Reset Detector Settings:
// Press RST twice within 2.5 seconds to toggle power modes.
#define DOUBLE_RESET_MAGIC           0xD00B1E01
#define DOUBLE_RESET_TIMEOUT_MS      2500

// RTC memory variables survive across hardware RST button presses
RTC_DATA_ATTR static uint32_t rtc_reset_magic = 0;
RTC_DATA_ATTR static uint8_t rtc_active_mode = 0;

static uint8_t active_power_mode = 0; // 0 = Continuous Low Power, 1 = Deep Sleep Burst
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

// Find My Device Network (FMDN) advertisement payload
uint8_t adv_raw_data[31] = {
    0x02,   // Length
    0x01,   // Flags data type value
    0x06,   // Flags data
    0x19,   // Length
    0x16,   // Service data data type value
    0xAA,   // 16-bit service UUID
    0xFE,   // 16-bit service UUID
    0x41,   // FMDN frame type with unwanted tracking protection mode indication
            // 20-byte ephemeral identifier (inserted in app_main)
            // Hashed flags (implicitly initialized to 0)
};

// Function to convert a hex string into a byte array
void hex_string_to_bytes(const char *hex, uint8_t *bytes, size_t len) {
    for (size_t i = 0; i < len; i++) {
        sscanf(hex + 2 * i, "%2hhx", &bytes[i]);
    }
}

// -------------------------------------------------------------------------
// 💡 Status LED Control & Mode Feedback
// -------------------------------------------------------------------------
static void led_init(void) {
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << STATUS_LED_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);
    gpio_set_level(STATUS_LED_PIN, !LED_ACTIVE_LEVEL); // Turn LED OFF
}

static void led_set(int on) {
    gpio_set_level(STATUS_LED_PIN, on ? LED_ACTIVE_LEVEL : !LED_ACTIVE_LEVEL);
}

// Blinks LED to indicate active mode:
//   Mode 0 (Continuous Low Power) -> 1 long blink (600ms)
//   Mode 1 (Deep Sleep Burst)     -> 2 rapid blinks (200ms each)
static void blink_mode_indicator(int mode) {
    if (mode == 0) {
        led_set(1);
        vTaskDelay(pdMS_TO_TICKS(600));
        led_set(0);
    } else {
        for (int i = 0; i < 2; i++) {
            led_set(1);
            vTaskDelay(pdMS_TO_TICKS(200));
            led_set(0);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
    }
}

// -------------------------------------------------------------------------
// 💾 NVS Flash Persistence (Remembers mode across battery disconnections)
// -------------------------------------------------------------------------
static uint8_t load_power_mode_from_nvs(void) {
    nvs_handle_t handle;
    uint8_t mode = 0;
    if (nvs_open("fmdn_cfg", NVS_READONLY, &handle) == ESP_OK) {
        nvs_get_u8(handle, "mode", &mode);
        nvs_close(handle);
    }
    return mode;
}

static void save_power_mode_to_nvs(uint8_t mode) {
    nvs_handle_t handle;
    if (nvs_open("fmdn_cfg", NVS_READWRITE, &handle) == ESP_OK) {
        nvs_set_u8(handle, "mode", mode);
        nvs_commit(handle);
        nvs_close(handle);
    }
}

// -------------------------------------------------------------------------
// 🔘 Double-Tap Reset Detection
// -------------------------------------------------------------------------
static void double_reset_timeout_task(void *pvParameters) {
    // Wait for the double-reset window to expire
    vTaskDelay(pdMS_TO_TICKS(DOUBLE_RESET_TIMEOUT_MS));
    rtc_reset_magic = 0; // Clear magic value
    ESP_LOGI(TAG, "Double-reset window expired. Mode locked.");
    vTaskDelete(NULL);
}

static void init_power_mode(void) {
    led_init();

    esp_reset_reason_t reason = esp_reset_reason();

    // If waking from Deep Sleep, do not trigger double-reset logic
    if (reason == ESP_RST_DEEPSLEEP) {
        active_power_mode = rtc_active_mode;
        return;
    }

    // Check if previous reset set the double-reset magic
    if (rtc_reset_magic == DOUBLE_RESET_MAGIC) {
        // Double-Reset Detected! Toggle mode
        rtc_reset_magic = 0;
        uint8_t current = load_power_mode_from_nvs();
        active_power_mode = (current == 0) ? 1 : 0;
        save_power_mode_to_nvs(active_power_mode);
        rtc_active_mode = active_power_mode;

        ESP_LOGW(TAG, "==================================================");
        ESP_LOGW(TAG, "⚡ DOUBLE-TAP RESET DETECTED!");
        ESP_LOGW(TAG, "Switched Power Mode to: %d (%s)",
                 active_power_mode,
                 active_power_mode == 0 ? "Continuous Low Power (3-6 weeks)" : "Deep Sleep Burst (3-6+ months)");
        ESP_LOGW(TAG, "==================================================");

        // Confirmation double-sequence blinks
        blink_mode_indicator(active_power_mode);
        vTaskDelay(pdMS_TO_TICKS(300));
        blink_mode_indicator(active_power_mode);
    } else {
        // Normal boot or single reset
        active_power_mode = load_power_mode_from_nvs();
        rtc_active_mode = active_power_mode;
        rtc_reset_magic = DOUBLE_RESET_MAGIC;

        ESP_LOGI(TAG, "Active Power Mode: %d (%s)",
                 active_power_mode,
                 active_power_mode == 0 ? "Continuous Low Power" : "Deep Sleep Burst");
        ESP_LOGI(TAG, "Tip: Double-tap RST button within 2.5s to toggle mode.");

        // Quick blink to show active mode on boot
        blink_mode_indicator(active_power_mode);

        // Start background timeout task to disarm double-reset after 2.5 seconds
        xTaskCreate(double_reset_timeout_task, "dbl_rst_tmr", 2048, NULL, 1, NULL);
    }
}

#if defined(CONFIG_IDF_TARGET_ESP32C3)
static int ble_advertise_cb(struct ble_gap_event *event, void *arg) {
    return 0;
}

static void ble_start_advertising(uint8_t *adv_raw_data, size_t adv_raw_data_len) {
    struct ble_gap_adv_params adv_params = {
        .conn_mode = BLE_GAP_CONN_MODE_NON,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = ADV_INTERVAL_UNITS,
        .itvl_max = ADV_INTERVAL_UNITS
    };

    ble_gap_adv_set_data(adv_raw_data, adv_raw_data_len);
    ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER, &adv_params, ble_advertise_cb, NULL);
    ESP_LOGI(TAG, "NimBLE advertising started (Interval: %d ms)", (int)(ADV_INTERVAL_UNITS * 0.625));
}

static void ble_host_task(void *param) {
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void on_sync(void) {
    ble_svc_gap_device_name_set("ESP32-C3-BLE");
    ble_start_advertising(adv_raw_data, sizeof(adv_raw_data));
}
#endif

// -------------------------------------------------------------------------
// 🚀 Main Application
// -------------------------------------------------------------------------
void app_main() {
    // 1. Initialize NVS (required for storage and BLE)
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 2. Initialize Double-Tap Reset Detector & Power Mode
    init_power_mode();

    // 3. Prepare FMDN advertisement data
    uint8_t eid_bytes[20];
    hex_string_to_bytes(eid_string, eid_bytes, 20);
    memcpy(&adv_raw_data[8], eid_bytes, 20);

    #if defined(CONFIG_IDF_TARGET_ESP32C3)
        ESP_LOGI(TAG, "Initializing NimBLE Stack");
        ESP_ERROR_CHECK(nimble_port_init());
        ble_hs_cfg.sync_cb = on_sync;
        ble_svc_gap_init();
        nimble_port_freertos_init(ble_host_task);

    #elif defined(CONFIG_IDF_TARGET_ESP32)
        // Initialize Bluetooth controller with low power / modem sleep configuration
        esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_bt_controller_init(&bt_cfg));
        ESP_ERROR_CHECK(esp_bt_controller_enable(ESP_BT_MODE_BLE));

        // Initialize Bluedroid stack
        ESP_ERROR_CHECK(esp_bluedroid_init());
        ESP_ERROR_CHECK(esp_bluedroid_enable());

        // Set optimized BLE TX power
        ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, BLE_TX_POWER_DEFAULT));
        ESP_ERROR_CHECK(esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_ADV, BLE_TX_POWER_DEFAULT));

        ESP_ERROR_CHECK(esp_ble_gap_config_adv_data_raw(adv_raw_data, sizeof(adv_raw_data)));

        // Configure advertisement parameters
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

    // 4. If in Mode 1 (Deep Sleep Burst Mode), cycle between broadcast and deep sleep
    if (active_power_mode == 1) {
        ESP_LOGI(TAG, "Deep Sleep Burst Mode active. Broadcasting for %d seconds...", BURST_ACTIVE_TIME_SEC);
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
    }
}