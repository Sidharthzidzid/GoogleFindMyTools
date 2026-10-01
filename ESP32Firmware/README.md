# Find My Device ESP32 Firmware

This code enables you to use an ESP32-device as a custom Google Find My Device tracker. Note that the firmware is very experimental. 

The firmware works differently to regular Find My Device trackers. It is made to be as simple as possible. It has no Fast Pair support, MAC rotation, advertisement rotation, etc.

Currently known working devices include the ESP32 (Dev Module V1), the ESP32-CAM, and ESP32-C3. If you use a different board and it works/doesn't work, feel free to message me, I'll update this README then. 


## How to use

- Run the Python Script [`main.py`](../main.py) in the parent folder. Follow the instructions of the [README of the parent folder](../README.md).
- When the device list is displayed, press 'r' to register a new ESP32/Zephyr device in your account. Copy the displayed advertisement key.
- Install Visual Studio Code [here](https://code.visualstudio.com/download)
- Go to Visual Studio Code Extensions, search, install and open 'ESP-IDF' by Espressif
- Open the folder containing this README file in Visual Studio Code
- Navigate to the folder main, select the file [`main.c`](main/main.c)
- Edit Line 15, and insert the advertisement key retrieved from the Python Script
- Connect your ESP32 to your system with USB
- On the bottom left of Visual Studio Code, click the 'plug' icon and select your ESP32, it should be named '/dev/tty.usbserial-0001' or similar
- Click the 'Fire' icon to build and flash the firmware
- If asked, use UART as flash method
- After flashing, the ESP32 will restart and start advertising as the Find My Device tracker previously registered


## Battery & Power Optimization Guide

The firmware includes a built-in **Double-Tap Reset Switcher** that lets you change power modes directly on the ESP32-CAM using the onboard **RST button**:

### 🔘 How to Switch Modes (Double-Tap Reset)
1. **Double-click the physical RST button** on the back of the ESP32-CAM within **2.5 seconds**.
2. The onboard **Red LED (GPIO 33)** on the back of the ESP32-CAM will blink to indicate the new mode:
   - **1 Long Blink**: **Mode 0 (Continuous Low Power)** — *3 to 6+ weeks battery life*
   - **2 Fast Blinks**: **Mode 1 (Deep Sleep Burst)** — *3 to 6+ months battery life*
3. The selected mode is **saved to flash memory (NVS)**, so the board remembers it even when disconnected from power or battery!

### 📊 Power Mode Comparison
- **Mode 0: Continuous Low Power** *(Default)*:
  - Broadcasts every **2.0 seconds** (`ADV_INTERVAL_UNITS = 3200`).
  - Radio modem sleep enabled between intervals; transmit power set to balanced **+3 dBm** (`ESP_PWR_LVL_P3`).
  - **Estimated battery life on 3200 mAh**: **3 to 6+ weeks**.
  - Best for everyday tracking (keys, backpack) so passing Android phones detect it immediately.

- **Mode 1: Deep Sleep Burst**:
  - Broadcasts for **5 seconds**, then enters ultra-low-power deep sleep for **30 seconds**.
  - **Estimated battery life on 3200 mAh**: **3 to 6+ months**.
  - Best for luggage, vehicles, or stationary items.

### ⚙️ Customizable Settings in [`main.c`](main/main.c)
```c
#define ADV_INTERVAL_UNITS       3200   // 3200 * 0.625ms = 2.0s advertising interval
#define BLE_TX_POWER_DEFAULT     ESP_PWR_LVL_P3 // +3 dBm balanced power
#define BURST_ACTIVE_TIME_SEC    5      // Active broadcast duration in seconds (for Mode 1)
#define DEEP_SLEEP_DURATION_SEC  30     // Deep sleep interval in seconds (for Mode 1)
#define STATUS_LED_PIN           33     // Onboard red LED on ESP32-CAM
```

## Known Issues

- You need to run [`main.py`](../main.py) every 4 days to keep receiving location reports from the server. This is because the advertisements have to be "announced" to Google. 
- Might not work with 'fresh' Google accounts: "Your encryption data is locked on your device" is shown if you have never paired a Find My Device tracker with an Android device. Solution: See [README of the parent folder](../README.md).
- You cannot view locations for the ESP32 in the Google Find My Device app. You will need to use the Python script to do so.
- No privacy features such as rotating MAC addresses are implemented
