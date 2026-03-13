#include "Display_SPD2010.h"
#include "Touch_SPD2010.h"
#include "Audio_PCM5101.h"
#include "RTC_PCF85063.h"
#include "LVGL_Driver.h"
#include "PWR_Key.h"
#include "SD_Card.h"
#include "BAT_Driver.h"
#include "Gyro_QMI8658.h"
#include "lvgl.h"
#include "src/ui.h"
#include "src/vars.h"
#include "src/screens.h"
#include "WiFi.h"
#include "time_client.h"
#include "geolocation.h"
#include "alarm_timer.h"
#include "weather.h"
#include "weather_locations.h"
#include "weather_location.h"
#include "serializable_config.h"
#include "serializable_configs.h"
#include "ble.h"
#include "notifications.h"
#include "wifi_client.h"
#include "settings.h"
#include "media_controls.h"
#include "calendar_fetcher.h"
#include "esp_core_dump.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "soc/rtc.h"

extern CalendarFetcher calendarFetcher;

uint8_t i2sBuffer[2048] __attribute__((section(".extram.bss")));

extern void monitor_heap();
extern void onWifiConnected();

Settings settings;
NotificationStore notificationStore;
GeoLocation ipLocation;
TimeClient timeClient;
Weather weather;
WeatherLocations weatherLocations;
AlarmTimer alarmTimer;
SerializableConfigs serializableConfigs;
BLE ble;
WiFi_Client wifiClient;
MediaControls mediaControls;

volatile bool locationDataReady = false;

void Driver_Loop(void *parameter) {
  unsigned long lastImuRead = 0;
  unsigned long lastSlowRead = 0;

  while (1) {
    // PWR_Loop always runs at 100ms — it drives the display wake/sleep state machine.
    PWR_Loop();

    bool awake = PWR_IsDisplayAwake();
    unsigned long now = millis();

    // Keep IMU cadence high enough for step detection even while display is off.
    // 100 ms awake, 125 ms asleep keeps good step sensitivity with lower CPU duty.
    unsigned long imuInterval = awake ? 100UL : 125UL;
    if (now - lastImuRead >= imuInterval) {
      lastImuRead = now;
      QMI8658_Loop();
    }

    // RTC/battery/time refresh does not need 10 Hz; reduce background wakeups.
    unsigned long slowInterval = awake ? 500UL : 2000UL;
    if (now - lastSlowRead >= slowInterval) {
      lastSlowRead = now;
      PCF85063_Loop();
      BAT_Get_Volts();
      timeClient.refresh();
    }

    // Monitor stack usage — kept at 8KB for safety, but measure for future optimization
    static unsigned long lastStackCheck = 0;
    if (millis() - lastStackCheck > 30000) {
      UBaseType_t stackRemaining = uxTaskGetStackHighWaterMark(NULL);
      if (BAT_Is_Charging()) {
        Serial.printf(">> Driver_Loop stack: %d bytes free\n",
                      stackRemaining * sizeof(StackType_t));
      }
      if (stackRemaining * sizeof(StackType_t) < 1024) {
        Serial.printf("!! WARNING: Driver_Loop stack low: %d bytes free !!\n",
                      stackRemaining * sizeof(StackType_t));
      }
      lastStackCheck = millis();
    }

    // 100 ms when awake for responsive UI; slower cadence when asleep reduces
    // background wakeups and gives the automatic PM/tickless-idle path longer
    // uninterrupted windows to enter light sleep safely.
    if (awake) {
      vTaskDelay(pdMS_TO_TICKS(100));
    } else {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

void Driver_Init() {
  Flash_test();
  PWR_Init();
  BAT_Init();
  I2C_Init();
  TCA9554PWR_Init(0x00);
  Backlight_Init();
  Set_Backlight(20);
  PCF85063_Init();
  QMI8658_Init();
}

// Periodic data sync interval — how often to reconnect and refresh network data.
// 120 minutes cuts radio duty cycle roughly in half versus hourly full sync.
static const unsigned long WIFI_SYNC_INTERVAL_MS = 120UL * 60UL * 1000UL;  // 120 minutes

void Background_Tasks(void *parameters) {
  // Trigger first cycle immediately to cover the WiFi already on from setup().
  unsigned long lastSyncTime = millis() - WIFI_SYNC_INTERVAL_MS;
  unsigned long lastRadioLog = 0;
  unsigned long lastDrainDiag = 0;
  unsigned long lastStateTs = millis();
  unsigned long wifiConnectedMs = 0;
  unsigned long wifiDisconnectedMs = 0;

  while (1) {
    unsigned long now = millis();
    bool wifiConnected = WiFi.isConnected();

    if (lastStateTs != 0) {
      unsigned long dt = now - lastStateTs;
      if (wifiConnected) wifiConnectedMs += dt;
      else wifiDisconnectedMs += dt;
    }
    lastStateTs = now;

    // Periodic sync: request a connection every 60 minutes so onWifiConnected()
    // refreshes NTP, location, weather, and calendar.  WiFi disconnects automatically
    // 30 seconds after keepAlive() is last called.
    if (now - lastSyncTime >= WIFI_SYNC_INTERVAL_MS) {
      lastSyncTime = now;
      Serial.println("WiFi sync: requesting periodic connection");
      wifiClient.keepAlive();

      // Keep WiFi alive while the calendar fetch is in progress.  Cap at 90 s
      // (was 5 min — a slow/unreachable server was keeping WiFi on for minutes
      // every sync cycle, which is a significant battery drain).
      unsigned long fetchWaitStart = millis();
      while (calendarFetcher.isFetchInProgress() &&
             millis() - fetchWaitStart < 90000UL) {
        wifiClient.keepAlive();           // Reset the 30-second idle window
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 s
      }
      Serial.printf("WiFi sync: calendar wait ended after %lus\n",
                    (millis() - fetchWaitStart) / 1000UL);
    }

    // Single lifecycle call: connects when keepAlive() has been called and WiFi is
    // down; disconnects automatically after 30 seconds of idle.
    wifiClient.processLifecycle();

    if (BAT_Is_Charging() && now - lastRadioLog >= 60000UL) {
      lastRadioLog = now;
      Serial.printf("[Radio] wifi_mode=%d wifi_connected=%d ble_ams=%d display_awake=%d\n",
                    (int)WiFi.getMode(),
                    WiFi.isConnected() ? 1 : 0,
                    ble.isAMSConnected() ? 1 : 0,
                    PWR_IsDisplayAwake() ? 1 : 0);
    }

    if (now - lastDrainDiag >= 300000UL) {  // every 5 minutes
      lastDrainDiag = now;
      unsigned long total = wifiConnectedMs + wifiDisconnectedMs;
      unsigned long wifiPct = total > 0 ? (wifiConnectedMs * 100UL) / total : 0;
      Serial.printf("[DrainDiag] batt=%s wifi_conn_pct=%lu%% wifi_mode=%d ble_ams=%d display_awake=%d\n",
                    BAT_Get_Charge_Percentage(),
                    wifiPct,
                    (int)WiFi.getMode(),
                    ble.isAMSConnected() ? 1 : 0,
                    PWR_IsDisplayAwake() ? 1 : 0);
      Serial.println("[PMDump] Active PM locks:");
      Serial.flush();
      fflush(stdout);
      esp_pm_dump_locks(stdout);
      fflush(stdout);
      Serial.println("[PMDump] ---end---");
      wifiConnectedMs = 0;
      wifiDisconnectedMs = 0;
    }

    // Monitor stack
    static unsigned long lastStackCheck = 0;
    now = millis();
    if (now - lastStackCheck > 30000) {
      UBaseType_t stackRemaining = uxTaskGetStackHighWaterMark(NULL);
      if (BAT_Is_Charging()) {
        Serial.printf(">> Background_Tasks stack high-water mark: %d bytes free\n",
                      stackRemaining * sizeof(StackType_t));
      }
      if (stackRemaining * sizeof(StackType_t) < 2000) {
        Serial.println("!! WARNING: Background_Tasks stack getting low !!");
      }
      lastStackCheck = now;
    }

    vTaskDelay(pdMS_TO_TICKS(1000));  // 1 s idle — no need to spin at 10 ms
  }
}

TaskHandle_t periodTasksHandle = NULL;
TaskHandle_t driverTaskHandle = NULL;
TaskHandle_t uiLoopTaskHandle = NULL;
TaskHandle_t bleTaskHandle = NULL;  // FIX: Dedicated BLE task, isolated from Wi-Fi tasks

void BLE_Task(void *parameter) {
  unsigned long now;

  while (1) {
    now = millis();

    ble.run();

    static unsigned long lastStackCheck = 0;
    if (now - lastStackCheck > 30000) {
      UBaseType_t stackRemaining = uxTaskGetStackHighWaterMark(NULL);
      if (BAT_Is_Charging()) {
        Serial.printf(">> BLE_Task stack high-water mark: %d bytes free\n",
                      stackRemaining * sizeof(StackType_t));
      }
      if (stackRemaining * sizeof(StackType_t) < 2000) {
        Serial.println("!! WARNING: BLE_Task stack getting low !!");
      }
      lastStackCheck = now;
    }

    // CRITICAL: Yield to IDLE0 so the task watchdog gets fed.
    // Without this, BLE_Task (priority 4) spins continuously and starves
    // IDLE0 (priority 0), causing a watchdog reboot every ~5 seconds when
    // ble.run() returns quickly (e.g. phone not connected).
    // 20 ms when display is on for responsive notifications/media.
    // 500-1000 ms when display is off to reduce CPU wakeups; NimBLE callbacks
    // still fire on their own and this only controls ble.run() polling cadence.
    bool awake = PWR_IsDisplayAwake();
    bool amsConnected = ble.isAMSConnected();
    if (awake) {
      vTaskDelay(pdMS_TO_TICKS(20));
    } else if (amsConnected) {
      vTaskDelay(pdMS_TO_TICKS(500));
    } else {
      vTaskDelay(pdMS_TO_TICKS(1000));
    }
  }
}

// UI loop task — replaces default loop() with 16KB stack (internal RAM)
void UI_Loop_Task(void *parameter) {
  while (1) {
    static unsigned long lastStackCheck = 0;
    if (millis() - lastStackCheck > 30000) {
      UBaseType_t stackRemaining = uxTaskGetStackHighWaterMark(NULL);
      if (BAT_Is_Charging()) {
        Serial.printf(">> UI loop stack high-water mark: %d bytes free\n",
                      stackRemaining * sizeof(StackType_t));
      }
      if (stackRemaining * sizeof(StackType_t) < 2000) {
        Serial.println("!! WARNING: UI loop stack getting low !!");
      }
      lastStackCheck = millis();
    }

    bool awake = PWR_IsDisplayAwake();

    if (awake) {
      // Rate-limit monitor_heap to 2 Hz — the internal 30 s gate handles the full
      // stats dump; the fast-path still calls ESP.getFreeHeap() every tick which is
      // unnecessary overhead at 200 Hz.
      static unsigned long lastHeapMonitor = 0;
      unsigned long _now = millis();
      if (_now - lastHeapMonitor >= 500) {
        lastHeapMonitor = _now;
        monitor_heap();
      }
      ui_tick();
    }

    if (awake) {
      Lvgl_Loop();  // LVGL rendering + input processing
    } else {
      // Display is off and LVGL tick timer is paused.  The SPD2010 touch
      // controller fires phantom/stale interrupts shortly after LCD_Sleep,
      // so discard any that arrive during the first 3 seconds.  After that
      // guard window, a touch interrupt is real and should wake the display.
      // Touch_HasPendingInterrupt() consumes the flag so a single phantom
      // interrupt only causes one (discarded) check, not a perpetual wake loop.
      if (Touch_HasPendingInterrupt()) {
        if (PWR_GetDisplaySleepMs() > 3000) {
          PWR_UpdateActivity();
        }
      }
    }

    if (awake) {
      static unsigned long lastWeatherUpdate = 0;
      if (millis() - lastWeatherUpdate > 5000) {
        lastWeatherUpdate = millis();
        weather.applyToUI();
      }

      // Check if calendar fetch completed and update display (must run on UI core)
      calendarFetcher.checkDisplayUpdate();

      if (notificationStore.getTotalCount() >= 0) {
        notificationStore.processCommands();
      }

      if (ble.mediaUIUpdateNeeded.exchange(false)) {
        if (ble.isAMSConnected()) {
          ble.updateMediaUIVariables();
        }
      }

      // FIX: Process buffered AMS entity update notifications.
      // The NimBLE AMS callback (Core 0) copies incoming entity-update packets
      // into amsUpdateBuffer and sets amsDataReady. processAMSEntityUpdate()
      // is documented "Called from Core 1" — it parses the buffer, writes to
      // writeBufferPtr, swaps display buffers, and calls updateMediaUIVariables()
      // (all of which require the LVGL/UI context of Core 1).
      // Without this call the buffer is filled but never consumed: track changes
      // are never reflected in the UI, amsNeedsTrackRefresh is never set, and
      // album artwork is never requested after the initial connection.
      ble.processAMSEntityUpdate();

      static unsigned long lastMediaUIUpdate = 0;
      if (millis() - lastMediaUIUpdate > 1000 && ble.isAMSConnected()) {
        lastMediaUIUpdate = millis();
        ble.updateMediaUIVariables();
      }

      mediaControls.apply_pending_artwork();

      wifiClient.asyncScanUpdate();
    }

    vTaskDelay(pdMS_TO_TICKS(awake ? 10 : 300));
  }
}

static void *psram_calloc(size_t n, size_t size) {
  void *ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ptr) {
    // Fallback to internal heap for small allocs or if PSRAM fails
    ptr = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
  }
  return ptr;
}

static void psram_free(void *ptr) {
  heap_caps_free(ptr);  // works for both PSRAM and internal pointers
}


void setup() {
  // Calibrate the internal fast RC oscillator for better sleep timing
  rtc_clk_fast_freq_set(RTC_FAST_FREQ_8M);

  // Latch power FIRST before any serial delays that would stall battery boot.
  // Driver_Init uses printf() (UART0) internally so it's safe before Serial.begin().
  Driver_Init();
  setCpuFrequencyMhz(80);  // Scale down from 240 MHz — cuts CPU power draw ~3x

  Serial.begin(115200);
  Serial.setTxBufferSize(2048);
  delay(200);
  // Only block waiting for a USB-CDC host when the device is on charge — on battery
  // boot there is no host and the 3-second wait wastes power needlessly.
  // BAT_Init() already ran inside Driver_Init(), so the ADC is ready for a direct read.
  {
    int _mv = analogReadMilliVolts(BAT_ADC_PIN);
    bool bootCharging = ((float)(_mv * 3.0f / 1000.0f) / Measurement_offset) > 4.0f;
    if (bootCharging) {
      unsigned long serialWait = millis();
      while (!Serial && millis() - serialWait < 3000) { delay(10); }  // key for native USB CDC
    }
  }

  int ret = mbedtls_platform_set_calloc_free(psram_calloc, psram_free);
  Serial.printf(">> mbedTLS → PSRAM: %s\n", ret == 0 ? "OK" : "FAILED");
  SD_Init();
  Audio_Init();
  LCD_Init();
  // Touch init can leave I2C in an invalid state on some boots; re-init bus and IMU
  // after LCD/Touch bring-up so step sensor reads remain valid.
  I2C_Init();
  QMI8658_Init();
  Serial.println(">> Reinitialized I2C + IMU after LCD/Touch init");
  Lvgl_Init();
  ui_init();

  serializableConfigs.add(alarmTimer);
  serializableConfigs.add(weatherLocations);
  serializableConfigs.add(settings);
  serializableConfigs.add(wifiClient);
  serializableConfigs.add(calendarFetcher);
  serializableConfigs.read();

  // Register callback to reload location/weather after WiFi connects
  wifiClient.setOnConnectedCallback(onWifiConnected);

  Serial.println("Starting smart WiFi connection...");
  if (!wifiClient.smartConnect(10000)) {
    Serial.println("Failed to auto-connect to WiFi");
  } else {
    Serial.println("WiFi auto-connected successfully!");
  }

  if (!notificationStore.begin()) {
    Serial.println("Failed to initialize notification store!");
  }

#ifndef DISABLE_BLE_FOR_PM_TEST
  ble.begin();
#else
  Serial.println(">> BLE DISABLED for light-sleep power test");
#endif

  if (WiFi.isConnected()) {
    locationDataReady = true;

    Serial.println("Requesting initial calendar fetch...");
    calendarFetcher.goToToday();
    calendarFetcher.requestFetch();
  }

  // FIX: Increased BLE_Task stack from 12KB to 20KB.
  // discoverAttributes() is the most stack-intensive BLE operation — it discovers
  // all services and characteristics in a single blocking call and allocates
  // significant stack-resident data structures. At 12KB the stack overflows when
  // BLE discovery coincides with a calendar HTTPS/TLS fetch on the same core,
  // corrupting adjacent memory and crashing during the next FreeRTOS context switch
  // (seen as LoadProhibited at EXCVADDR 0x00000043 in tasks.c/esp_adapter.c).
  // FIX: Use xTaskCreatePinnedToCoreWithCaps with MALLOC_CAP_SPIRAM for all task stacks.
  // Requires CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y in sdkconfig.ext.
  // On ESP32-S3 (LX7), PSRAM task stacks work correctly via the MMU cache — the old
  // comment about "Xtensa register window save requires internal RAM" applied to the
  // original ESP32 (LX6) without cache. Moving stacks to PSRAM frees ~42KB of internal
  // heap that was previously consumed by four permanent task stacks, plus eliminates
  // the 32KB one-shot CalFetch stack that was causing the 9KB min-free-heap crisis.

  // FIX APPLIED: Stack was documented as 20KB in comment but code still had 12288 (12KB).
  // discoverAttributes() during BLE connection setup is the most stack-intensive path —
  // it allocates large temporary structures on the stack and can overflow 12KB when the
  // BLE host task's own call frames are already deep. 20KB matches the documented intent.
#ifndef DISABLE_BLE_FOR_PM_TEST
  xTaskCreatePinnedToCoreWithCaps(
    BLE_Task,
    "BLE Task",
    16384,
    NULL,
    4,                // Higher priority than Background_Tasks (3)
    &bleTaskHandle,
    0,                // Core 0 — same as NimBLE host task
    MALLOC_CAP_SPIRAM);
  Serial.println(">> BLE task created (Core 0, priority 4, 16KB PSRAM stack)");
#endif

  // Background_Tasks now also runs calendar fetch directly (no CalFetch one-shot task).
  // 32KB stack gives plenty of room for TLS handshake call frames for both weather
  // and calendar HTTPS — costs nothing from internal heap since stack is in PSRAM.
  xTaskCreatePinnedToCoreWithCaps(
    Background_Tasks,
    "Background Tasks",
    20480,
    NULL,
    3,
    &periodTasksHandle,
    0,
    MALLOC_CAP_SPIRAM);
  Serial.println(">> Background Tasks created (Core 0, priority 3, 20KB PSRAM stack)");    

  xTaskCreatePinnedToCoreWithCaps(
    Driver_Loop,
    "Driver Task",
    6144,
    NULL,
    3,
    &driverTaskHandle,
    0,
    MALLOC_CAP_SPIRAM);

  xTaskCreatePinnedToCoreWithCaps(
    UI_Loop_Task,
    "UI Loop",
    16384,
    NULL,
    1,
    &uiLoopTaskHandle,
    1,
    MALLOC_CAP_SPIRAM);
  Serial.printf(">> All tasks created with PSRAM stacks — internal heap preserved\n");

  // -----------------------------------------------------------------------
  // Automatic light sleep via FreeRTOS tickless idle + BLE modem sleep.
  //
  // ESP-IDF libs were rebuilt (HybridCompile) with:
  //   CONFIG_PM_ENABLE=y, CONFIG_FREERTOS_USE_TICKLESS_IDLE=y,
  //   CONFIG_BT_CTRL_MODEM_SLEEP=y, CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL=y
  //
  // BLE modem sleep uses the main 40MHz crystal as LP clock (no external
  // 32.768 kHz crystal needed). The BLE controller sleeps between events
  // and should NOT hold the btLS NO_LIGHT_SLEEP PM lock.
  //
  // DFS range: 40-80 MHz. Light sleep engages when all tasks are idle.
  // -----------------------------------------------------------------------
  // Light sleep kills the native USB-CDC peripheral (no independent clock),
  // so disable it when USB is connected to keep /dev/ttyACM0 alive.
  // BAT_Is_Charging() isn't reliable yet (Driver_Loop hasn't run), so read ADC directly.
  int _pmMv = analogReadMilliVolts(BAT_ADC_PIN);
  bool usbConnected = ((float)(_pmMv * 3.0f / 1000.0f) / Measurement_offset) > 4.0f;
  bool allow_light_sleep = !usbConnected;
  esp_pm_config_t pm_config = {
    .max_freq_mhz       = 80,
    .min_freq_mhz       = 40,
    .light_sleep_enable = allow_light_sleep,
  };
  esp_err_t pm_err = esp_pm_configure(&pm_config);
  if (pm_err == ESP_ERR_NOT_SUPPORTED) {
    pm_config.light_sleep_enable = false;
    pm_err = esp_pm_configure(&pm_config);
    if (pm_err == ESP_OK) {
      Serial.println(">> Power management: DFS enabled (40-80MHz), light sleep not supported");
    }
  }
  if (pm_err != ESP_OK) {
    Serial.printf(">> Power management setup failed: %s\n", esp_err_to_name(pm_err));
  } else if (pm_config.light_sleep_enable) {
    Serial.println(">> Power management: DFS + automatic light sleep enabled");
  } else if (allow_light_sleep == false) {
    Serial.println(">> Power management: DFS enabled, light sleep disabled (USB connected)");
  }

  // Dump PM locks at boot to check if btLS is still held
  Serial.println("[PMDump] Boot PM locks:");
  Serial.flush();
  fflush(stdout);
  esp_pm_dump_locks(stdout);
  fflush(stdout);
  Serial.println("[PMDump] ---end---");
}


void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
