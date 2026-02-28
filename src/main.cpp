#include "Display_SPD2010.h"
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
  unsigned long lastSensorRead = 0;

  while (1) {
    // PWR_Loop always runs at 100ms — it drives the display wake/sleep state machine.
    PWR_Loop();

    // Sensor and time reads only need ~2Hz when display is off.
    // When display is on, run at full 10Hz (every 100ms).
    unsigned long sensorInterval = PWR_IsDisplayAwake() ? 100UL : 500UL;
    unsigned long now = millis();
    if (now - lastSensorRead >= sensorInterval) {
      lastSensorRead = now;
      QMI8658_Loop();
      PCF85063_Loop();
      BAT_Get_Volts();
      timeClient.refresh();
    }

    // Monitor stack usage — kept at 8KB for safety, but measure for future optimization
    static unsigned long lastStackCheck = 0;
    if (millis() - lastStackCheck > 30000) {
      UBaseType_t stackRemaining = uxTaskGetStackHighWaterMark(NULL);
      Serial.printf(">> Driver_Loop stack: %d bytes free\n",
                    stackRemaining * sizeof(StackType_t));
      if (stackRemaining * sizeof(StackType_t) < 1024) {
        Serial.printf("!! WARNING: Driver_Loop stack low: %d bytes free !!\n",
                      stackRemaining * sizeof(StackType_t));
      }
      lastStackCheck = millis();
    }

    vTaskDelay(pdMS_TO_TICKS(100));
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

// Periodic data sync interval — how often to reconnect and refresh NTP/geo/weather/calendar.
static const unsigned long WIFI_SYNC_INTERVAL_MS = 15UL * 60UL * 1000UL;  // 15 minutes

void Background_Tasks(void *parameters) {
  // Trigger first cycle immediately to cover the WiFi already on from setup().
  unsigned long lastSyncTime = millis() - WIFI_SYNC_INTERVAL_MS;

  while (1) {
    unsigned long now = millis();

    // Periodic sync: request a connection every 15 minutes so onWifiConnected()
    // refreshes NTP, location, weather, and calendar.  WiFi disconnects automatically
    // 30 seconds after keepAlive() is last called.
    if (now - lastSyncTime >= WIFI_SYNC_INTERVAL_MS) {
      lastSyncTime = now;
      Serial.println("WiFi sync: requesting periodic connection");
      wifiClient.keepAlive();

      // Keep WiFi alive while the calendar fetch is in progress.  Cap at 60 s.
      unsigned long fetchWaitStart = millis();
      while (calendarFetcher.isFetchInProgress() &&
             millis() - fetchWaitStart < 60000UL) {
        wifiClient.keepAlive();           // Reset the 30-second idle window
        vTaskDelay(pdMS_TO_TICKS(5000));  // Check every 5 s
      }
    }

    // Single lifecycle call: connects when keepAlive() has been called and WiFi is
    // down; disconnects automatically after 30 seconds of idle.
    wifiClient.processLifecycle();

    // Monitor stack
    static unsigned long lastStackCheck = 0;
    now = millis();
    if (now - lastStackCheck > 30000) {
      UBaseType_t stackRemaining = uxTaskGetStackHighWaterMark(NULL);
      Serial.printf(">> Background_Tasks stack high-water mark: %d bytes free\n",
                    stackRemaining * sizeof(StackType_t));
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
      Serial.printf(">> BLE_Task stack high-water mark: %d bytes free\n",
                    stackRemaining * sizeof(StackType_t));
      if (stackRemaining * sizeof(StackType_t) < 2000) {
        Serial.println("!! WARNING: BLE_Task stack getting low !!");
      }
      lastStackCheck = now;    
    }

    // CRITICAL: Yield to IDLE0 so the task watchdog gets fed.
    // Without this, BLE_Task (priority 4) spins continuously and starves
    // IDLE0 (priority 0), causing a watchdog reboot every ~5 seconds when
    // ble.run() returns quickly (e.g. phone not connected).
    // 10 ms (100 Hz) when display is on for responsive notifications/media.
    // 100 ms (10 Hz) when display is off — NimBLE callbacks still fire on
    // their own; this only controls how often we poll ble.run() for work.
    vTaskDelay(pdMS_TO_TICKS(PWR_IsDisplayAwake() ? 10 : 100));
  }
}

// UI loop task — replaces default loop() with 16KB stack (internal RAM)
void UI_Loop_Task(void *parameter) {
  while (1) {
    static unsigned long lastStackCheck = 0;
    if (millis() - lastStackCheck > 30000) {
      UBaseType_t stackRemaining = uxTaskGetStackHighWaterMark(NULL);
      Serial.printf(">> UI loop stack high-water mark: %d bytes free\n",
                    stackRemaining * sizeof(StackType_t));
      if (stackRemaining * sizeof(StackType_t) < 2000) {
        Serial.println("!! WARNING: UI loop stack getting low !!");
      }
      lastStackCheck = millis();
    }

    monitor_heap();

    ui_tick();
    Lvgl_Loop();

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

    vTaskDelay(pdMS_TO_TICKS(5));
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
  // Latch power FIRST before any serial delays that would stall battery boot.
  // Driver_Init uses printf() (UART0) internally so it's safe before Serial.begin().
  Driver_Init();
  setCpuFrequencyMhz(80);  // Scale down from 240 MHz — cuts CPU power draw ~3x

  Serial.begin(115200);
  Serial.setTxBufferSize(2048);
  delay(200);
  unsigned long serialWait = millis();
  while (!Serial && millis() - serialWait < 3000) { delay(10); }  // key for native USB CDC

  int ret = mbedtls_platform_set_calloc_free(psram_calloc, psram_free);
  Serial.printf(">> mbedTLS → PSRAM: %s\n", ret == 0 ? "OK" : "FAILED");
  SD_Init();
  Audio_Init();
  LCD_Init();
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

  ble.begin();

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
}


// All work moved to UI_Loop_Task. Default loop() idles.
void loop() {  
  vTaskDelay(pdMS_TO_TICKS(1000));
}
