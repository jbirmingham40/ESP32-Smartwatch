#include "src/eez-flow.h"
#include "src/screens.h"
#include "src/images.h"
#include "src/fonts.h"
#include "src/actions.h"
#include "src/vars.h"
#include "src/styles.h"
#include "src/ui.h"
#include <string.h>
#include <algorithm>
#include <vector>
#include "WiFi.h"
#include "wifi_client.h"
#include "serializable_configs.h"
#include "geolocation.h"
#include "time_client.h"
#include "weather.h"
#include "calendar_fetcher.h"

extern WiFi_Client wifiClient;
extern SerializableConfigs serializableConfigs;
extern GeoLocation ipLocation;
extern TimeClient timeClient;
extern Weather weather;
extern CalendarFetcher calendarFetcher;

bool wifiScanInProgress = false;
unsigned long wifiScanStartTime = 0;
const unsigned long WIFI_SCAN_TIMEOUT_MS = 15000;  // 15 second timeout
uint32_t wifiScreenGeneration = 0;
uint32_t wifiScanGeneration = 0;
bool wifiScreenActive = false;
WifiUiScanResult *pendingWifiUiResult = nullptr;
bool wifiAsyncReconnectNeeded = false;

// WiFi auto-reconnect variables
static unsigned long lastWiFiCheck = 0;
static unsigned long lastConnectAttempt = 0;
static const unsigned long WIFI_CHECK_INTERVAL = 5000;     // Check every 5 seconds
static const unsigned long WIFI_RETRY_INTERVAL = 60000;    // Retry every 60 seconds
static bool wifiWasConnected = false;

// External flag from main .ino file
extern volatile bool locationDataReady;

// Callback function to reload location and weather after WiFi connection
void onWifiConnected() {
  Serial.println("=== WiFi Connected - Reloading time, location, calendar and weather ===");
  
  // FIRST: Sync time with NTP (fast, gets accurate time immediately)
  timeClient.begin();
  Serial.println("Time synced with NTP");

  
  // SECOND: Reload location using IP (slow HTTP call ~7 seconds)
  if (ipLocation.loadUsingIp()) {
    Serial.println("Location reloaded successfully");
    locationDataReady = true;  // Set flag after successful location load

    // THIRD: Update timezone based on new location
    timeClient.lookupTimezone(ipLocation.getLatitude(), ipLocation.getLongitude());
  } else {
    Serial.println("Failed to reload location");
    locationDataReady = false;  // Clear flag on failure
  }
  
  // FOURTH: Reload calendar data
  calendarFetcher.requestFetch();

  // FIFTH: Reload weather data (only if location loaded successfully)
  if (locationDataReady) {
    weather.loadWeather();
  }

  Serial.println("=== Post-connection reload complete ===");
}

// Async callbacks that run on LVGL thread
static void asyncPushScreen(void *param) {
  ScreenOperation *op = (ScreenOperation *)param;
  if (op) {
    eez_flow_push_screen(op->screenId, op->animType, op->time, op->delay);
    lv_mem_free(op);
  }
}

static void asyncPopScreen(void *param) {
  ScreenOperation *op = (ScreenOperation *)param;
  if (op) {
    eez_flow_pop_screen(op->animType, op->time, op->delay);
    lv_mem_free(op);
  }
}

void action_wifi_network_clicked(lv_event_t *e) {
  lv_obj_t *row_container = lv_event_get_target(e);
  lv_obj_t *ssid_container = lv_obj_get_child(row_container, 0);

  lv_obj_t *ssid_label = lv_obj_get_child(ssid_container, 0);
  const char *ssid = lv_label_get_text(ssid_label);

  lv_obj_t *rssisec_container = lv_obj_get_child(row_container, 1);
  bool isOpen = lv_obj_get_child_cnt(rssisec_container) == 1;

  bool isSaved = wifiClient.isNetworkSaved(ssid);

  if (!isSaved) {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_PASSWORD, eez::StringValue(""));
  } else {
    // CRITICAL FIX: Avoid String allocation - use const char* directly
    const char *savedPassword = wifiClient.getPasswordForSavedSsid(ssid);
    Serial.printf("Looking up password for %s / %s\n", ssid, savedPassword ? savedPassword : "(null)");
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_PASSWORD, eez::StringValue(savedPassword ? savedPassword : ""));
  }

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_SSID, eez::StringValue(ssid));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_IS_OPEN, eez::BooleanValue(isOpen));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_IS_SAVED, eez::BooleanValue(isSaved));

  Serial.printf("wifi network clicked %s - %s - %s\n", ssid, isOpen ? "open" : "secure", isSaved ? "saved" : "not saved");

  // Safe to call from event handler - defers until next LVGL tick
  ScreenOperation *op = (ScreenOperation *)lv_mem_alloc(sizeof(ScreenOperation));
  if (op) {
    op->screenId = SCREEN_ID_WIFI_CREDENTIALS;
    op->animType = LV_SCR_LOAD_ANIM_MOVE_LEFT;
    op->time = 200;
    op->delay = 0;
    lv_async_call(asyncPushScreen, op);
  }
}

static void insert_wifi_row(const int rssi, const bool isOpen, const char *ssid) {

  lv_obj_t *parent_list = objects.wifi_available_network_list;

  // Container: list row
  lv_obj_t *list_row_container = lv_obj_create(parent_list);
  lv_obj_set_width(list_row_container, LV_PCT(100));
  lv_obj_set_height(list_row_container, LV_SIZE_CONTENT);
  lv_obj_set_style_layout(list_row_container, LV_LAYOUT_FLEX, LV_PART_MAIN);
  lv_obj_set_style_flex_flow(list_row_container, LV_FLEX_FLOW_ROW, LV_PART_MAIN);
  lv_obj_set_style_flex_main_place(list_row_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_PART_MAIN);
  lv_obj_set_style_flex_cross_place(list_row_container, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
  lv_obj_set_style_border_width(list_row_container, 1, LV_PART_MAIN);
  lv_obj_set_style_border_side(list_row_container, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);
  lv_obj_set_style_pad_top(list_row_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(list_row_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(list_row_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_right(list_row_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(list_row_container, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_column(list_row_container, 10, LV_PART_MAIN);
  lv_obj_add_event_cb(list_row_container, action_wifi_network_clicked, LV_EVENT_CLICKED, NULL);

  // Container: ssid & saved container
  lv_obj_t *ssid_container = lv_obj_create(list_row_container);
  lv_obj_set_width(ssid_container, LV_SIZE_CONTENT);
  lv_obj_set_height(ssid_container, LV_SIZE_CONTENT);
  lv_obj_set_style_layout(ssid_container, LV_LAYOUT_FLEX, LV_PART_MAIN);
  lv_obj_set_style_flex_flow(ssid_container, LV_FLEX_FLOW_COLUMN, LV_PART_MAIN);
  lv_obj_set_style_pad_top(ssid_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(ssid_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(ssid_container, 5, LV_PART_MAIN);
  lv_obj_set_style_pad_right(ssid_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_row(ssid_container, 4, LV_PART_MAIN);
  lv_obj_set_style_pad_column(ssid_container, 10, LV_PART_MAIN);
  lv_obj_set_style_border_width(ssid_container, 0, LV_PART_MAIN);

  // Label: ssid
  lv_obj_t *ssid_label = lv_label_create(ssid_container);
  lv_obj_set_width(ssid_label, LV_SIZE_CONTENT);
  lv_obj_set_height(ssid_label, LV_SIZE_CONTENT);
  lv_label_set_text(ssid_label, ssid);
  lv_obj_set_style_border_width(ssid_label, 0, LV_PART_MAIN);

  if (wifiClient.isNetworkSaved(ssid)) {
    // Container: green check and saved
    lv_obj_t *saved_container = lv_obj_create(ssid_container);
    lv_obj_set_style_layout(saved_container, LV_LAYOUT_FLEX, LV_PART_MAIN);
    lv_obj_set_width(saved_container, LV_SIZE_CONTENT);
    lv_obj_set_height(saved_container, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_top(saved_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_bottom(saved_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_left(saved_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_right(saved_container, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_row(saved_container, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_column(saved_container, 5, LV_PART_MAIN);
    lv_obj_set_style_border_width(saved_container, 0, LV_PART_MAIN);

    // Image: green check
    lv_obj_t *check_image = lv_img_create(saved_container);
    lv_obj_set_width(check_image, LV_SIZE_CONTENT);
    lv_obj_set_height(check_image, LV_SIZE_CONTENT);
    lv_img_set_src(check_image, &img_greencheck);
    lv_img_set_zoom(check_image, 200);
    lv_obj_set_style_border_width(check_image, 0, LV_PART_MAIN);

    // Label: saved
    lv_obj_t *saved_label = lv_label_create(saved_container);
    lv_label_set_text(saved_label, "Saved");
    lv_obj_set_width(saved_label, LV_SIZE_CONTENT);
    lv_obj_set_height(saved_label, LV_SIZE_CONTENT);
    lv_obj_set_style_text_color(saved_label, lv_color_hex(0xffacacac), LV_PART_MAIN);
    lv_obj_set_style_text_font(saved_label, &lv_font_montserrat_12, LV_PART_MAIN);
    lv_obj_set_style_border_width(saved_label, 0, LV_PART_MAIN);
  }

  // Container: secure and rssi
  lv_obj_t *rssisec_container = lv_obj_create(list_row_container);
  lv_obj_set_width(rssisec_container, LV_SIZE_CONTENT);
  lv_obj_set_height(rssisec_container, LV_SIZE_CONTENT);
  lv_obj_set_style_layout(rssisec_container, LV_LAYOUT_FLEX, LV_PART_MAIN);
  lv_obj_set_style_pad_top(rssisec_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_bottom(rssisec_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_left(rssisec_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_right(rssisec_container, 8, LV_PART_MAIN);
  lv_obj_set_style_pad_row(rssisec_container, 10, LV_PART_MAIN);
  lv_obj_set_style_pad_column(rssisec_container, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(rssisec_container, 0, LV_PART_MAIN);

  if (!isOpen) {
    lv_obj_t *secure_image = lv_img_create(rssisec_container);
    lv_obj_set_width(secure_image, LV_SIZE_CONTENT);
    lv_obj_set_height(secure_image, LV_SIZE_CONTENT);
    lv_img_set_src(secure_image, &img_lock_24);
    lv_img_set_zoom(secure_image, 200);
    lv_obj_set_style_border_width(secure_image, 0, LV_PART_MAIN);
  }

  lv_obj_t *rssi_image = lv_img_create(rssisec_container);
  lv_obj_set_width(rssi_image, LV_SIZE_CONTENT);
  lv_obj_set_height(rssi_image, LV_SIZE_CONTENT);
  lv_obj_set_style_border_width(rssi_image, 0, LV_PART_MAIN);

  if (rssi > -25.0f) lv_img_set_src(rssi_image, &img_wifi_24);
  else if (rssi > -50.0f) lv_img_set_src(rssi_image, &img_wifi_three_24);
  else if (rssi > -75.0f) lv_img_set_src(rssi_image, &img_wifi_two_24);
  else lv_img_set_src(rssi_image, &img_wifi_one_24);
}

static void applyWifiScanResultToUi(void *p) {
  WifiUiScanResult *r = (WifiUiScanResult *)p;
  if (!r) return;

  // Screen navigated away or rebuilt since the scan started? Ignore.
  if (r->gen != wifiScreenGeneration) {
    lv_mem_free(r);
    return;
  }

  // Get the parent container where items will be added
  // Container: wifi_available_network_list
  lv_obj_t *parent_list = objects.wifi_available_network_list;

  // Parent may have been deleted even if our generation token matches (e.g. screen
  // rebuilt). Guard against use-after-free.
  if (parent_list == nullptr || !lv_obj_is_valid(parent_list)) {
    lv_mem_free(r);
    return;
  }

  // Clear any previous rows before rebuilding
  lv_obj_clean(parent_list);

  for (int i = 0; i < r->count; i++) {
    insert_wifi_row(r->nets[i].rssi, r->nets[i].isOpen, r->nets[i].ssid);
  }

  Serial.println(">> Updated wifi_available_list");
  lv_mem_free(r);
}

// Call from your WiFi screen's LVGL event handlers.
void action_wifi_screen_loaded(lv_event_t *e) {
  wifiScreenActive = true;
  wifiScreenGeneration++;

  // If we have a pending result from a scan that finished while off-screen,
  // apply it now on the LVGL thread (using the current generation).
  if (pendingWifiUiResult) {
    pendingWifiUiResult->gen = wifiScreenGeneration;
    lv_async_call(applyWifiScanResultToUi, pendingWifiUiResult);
    pendingWifiUiResult = nullptr;
  }
}

void action_wifi_screen_unloaded(lv_event_t *e) {
  wifiScreenActive = false;
  wifiScreenGeneration++;

  // Optional: drop any pending UI result when leaving the screen to free memory.
  if (pendingWifiUiResult) {
    lv_mem_free(pendingWifiUiResult);
    pendingWifiUiResult = nullptr;
  }
}

void action_forget_wifi_network(lv_event_t *e) {
  String ssid = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_SSID).getString();
  Serial.printf("Forget network %s\n", ssid.c_str());

  // Check if we're forgetting the currently connected network
  if (WiFi.isConnected() && strcmp(wifiClient.getSsid(), ssid.c_str()) == 0) {
    Serial.printf("Disconnecting from current network %s before forgetting\n", ssid.c_str());
    WiFi.disconnect();
    wifiClient.setSsid("");
    wifiClient.setPassword("");
  }
  
  // Remove from saved networks
  wifiClient.removeSavedNetwork(ssid.c_str());
}

void action_join_wifi_network(lv_event_t *e) {
  String ssid = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_SSID).getString();
  String password = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_PASSWORD).getString();

  Serial.printf("Join network %s with password %s\n", ssid.c_str(), password.c_str());
  
  // Attempt to join the network with 10 second timeout
  bool success = wifiClient.joinNetwork(ssid.c_str(), password.c_str(), 10000);
  
  if (success) {
    Serial.printf("Successfully joined and saved network %s\n", ssid.c_str());
    // Update the saved status since we just saved it
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_IS_SAVED, eez::BooleanValue(true));   
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_JOIN_SUCCESS, eez::BooleanValue(true));
  } else {
    Serial.printf("Failed to join network %s\n", ssid.c_str());    
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WIFI_CREDS_JOIN_SUCCESS, eez::BooleanValue(false));
  }
  
  // The UI can check get_var_wifi_connected() to determine current connection status
}

void action_load_available_wifi_networks(lv_event_t *e) {
  // Start an ASYNC scan - this returns immediately and doesn't block LVGL
  // The scan results will be processed in wifi_process() called from the main loop

  if (wifiScanInProgress) {
    Serial.println(">> WiFi scan already in progress");
    return;
  }

  Serial.println(">> Starting async WiFi scan...");

  // Capture the generation at scan start so we can ignore stale results if the user
  // navigates away before the scan completes.
  wifiScanGeneration = wifiScreenGeneration;

  // Start async scan with faster timing:
  // scanNetworks(async, show_hidden, passive, max_ms_per_chan, channel)
  // - async = true (non-blocking)
  // - show_hidden = false (skip hidden networks for speed)
  // - passive = false (active scanning finds more networks)
  // - max_ms_per_chan = 100ms (default is 300ms) - reduce for speed
  // - channel = 0 (scan all channels)
  //
  // With 100ms per channel * 13 channels = ~1.3 seconds total
  // You can go as low as 50ms but may miss some networks
  WiFi.scanNetworks(true, false, false, 100);

  wifiScanInProgress = true;
  wifiScanStartTime = millis();
}

const char *get_var_wifi_ssid() {
  return wifiClient.getSsid();
}

void set_var_wifi_ssid(const char *value) {
}

int32_t get_var_wifi_signal_strength() {
  static float signalStrength = WiFi.RSSI();
  static time_t lastUpdate = time(0);

  // only update every 20 seconds
  if (lastUpdate - time(0) > 20000) {
    if (signalStrength > -25.0f) return 4;
    else if (signalStrength > -50.0f) return 3;
    else if (signalStrength > -75.0f) return 2;
    else return 1;
  } else {
    return signalStrength;
  }
}

void set_var_wifi_signal_strength(int32_t value) {
}

bool get_var_wifi_connected() {
  return wifiClient.isConnected();
}

void set_var_wifi_connected(bool value) {
}

bool WiFi_Client::isConnected() {
  return WiFi.isConnected();
}

void WiFi_Client::setSsid(const char *ssid) {
  if (ssid == nullptr) {
    this->ssid[0] = '\0';
    return;
  }
  strncpy(this->ssid, ssid, sizeof(this->ssid) - 1);
  this->ssid[sizeof(this->ssid) - 1] = '\0';  // Ensure null termination
}

const char *WiFi_Client::getSsid() {
  return ssid;
}

void WiFi_Client::setPassword(const char *password) {
  if (password == nullptr) {
    this->password[0] = '\0';
    return;
  }
  strncpy(this->password, password, sizeof(this->password) - 1);
  this->password[sizeof(this->password) - 1] = '\0';  // Ensure null termination
}

const char *WiFi_Client::getPassword() {
  if (strlen(password) == 0) {
    return NULL;
  }
  return password;
}

void WiFi_Client::startWifi() {
  static bool wifiStartRunning = false;

  if(wifiStartRunning) {
    Serial.println("Skipping start wifi since it is already running");
    return;
  } else {
    Serial.println("Starting wifi");
    wifiStartRunning = true;
  }

  const char *ssid_str = getSsid();
  if (strlen(ssid_str) == 0) {
    Serial.println("No ssid selected.  Wifi not available.");
    return;
  }

  WiFi.begin(getSsid(), getPassword());
  Serial.println(F("Connecting Wifi"));

  int retries = 20;
  while (retries > 0 && WiFi.status() != WL_CONNECTED) {
    Serial.print(F("."));
    retries--;
    delay(500);
  }

  if (WiFi.status() != WL_CONNECTED) {
    Serial.printf("Unable to connect to access point %s\n", ssid_str);
    //eez::flow::setUserProperty(ACTION_JOIN_WIFI_NETWORK_PROPERTY_SUCCESS, eez::BooleanValue(false));
    return;
  }

  Serial.printf("Connected to access point %s. Wifi RSSI=%d\n", ssid_str, WiFi.RSSI());

  // CRITICAL FIX: Save network using vector instead of unordered_map
  // Check if network already exists
  int idx = findNetworkIndex(ssid_str);
  if (idx >= 0) {
    // Update existing password
    strncpy(savedNetworks[idx].password, password, sizeof(savedNetworks[idx].password) - 1);
    savedNetworks[idx].password[sizeof(savedNetworks[idx].password) - 1] = '\0';
  } else {
    // Add new network
    WifiSavedNetwork newNetwork;
    strncpy(newNetwork.ssid, ssid_str, sizeof(newNetwork.ssid) - 1);
    newNetwork.ssid[sizeof(newNetwork.ssid) - 1] = '\0';
    strncpy(newNetwork.password, password, sizeof(newNetwork.password) - 1);
    newNetwork.password[sizeof(newNetwork.password) - 1] = '\0';
    savedNetworks.push_back(newNetwork);
  }
  
  serializableConfigs.write();
}

// CRITICAL FIX: Helper to find network index in vector
int WiFi_Client::findNetworkIndex(const char *ssid) {
  for (size_t i = 0; i < savedNetworks.size(); i++) {
    if (strcmp(savedNetworks[i].ssid, ssid) == 0) {
      return (int)i;
    }
  }
  return -1;  // Not found
}

// CRITICAL FIX: Return const char* instead of String& to avoid heap allocation
const char *WiFi_Client::getPasswordForSavedSsid(const char *ssid) {
  int idx = findNetworkIndex(ssid);
  if (idx >= 0) {
    return savedNetworks[idx].password;
  }
  return "";  // Return empty string if not found
}

bool WiFi_Client::isNetworkSaved(const char *lookupSsid) {
  Serial.printf("Looking up network %s - ", lookupSsid);
  int idx = findNetworkIndex(lookupSsid);
  bool saved = (idx >= 0);
  Serial.printf("%s\n", saved ? "saved" : "not saved");
  return saved;
}

// Keep for backward compatibility but avoid using in new code
bool WiFi_Client::isNetworkSaved(String lookupSsid) {
  return isNetworkSaved(lookupSsid.c_str());
}

void WiFi_Client::removeSavedNetwork(const char *ssid) {
  int idx = findNetworkIndex(ssid);
  if (idx >= 0) {
    savedNetworks.erase(savedNetworks.begin() + idx);
    serializableConfigs.write();
  }
}

void WiFi_Client::asyncReconnect() {
  unsigned long now = millis();
  
  // Throttle checks to every 5 seconds
  if (now - lastWiFiCheck < WIFI_CHECK_INTERVAL) {
    return;
  }
  lastWiFiCheck = now;
  
  bool isConnected = WiFi.isConnected();
  
  // Track connection state changes
  if (isConnected && !wifiWasConnected) {
    Serial.println("WiFi connection established");
    wifiWasConnected = true;
  } else if (!isConnected && wifiWasConnected) {
    Serial.println("WiFi connection lost!");
    wifiWasConnected = false;
  }
  
  // If disconnected and have saved networks, try to reconnect every 60 seconds
  if (!isConnected && savedNetworks.size() > 0) {
    // Don't retry too frequently
    if (now - lastConnectAttempt < WIFI_RETRY_INTERVAL) {
      return;
    }
    
    Serial.println("WiFi disconnected - attempting smart reconnect...");
    lastConnectAttempt = now;
    
    // Attempt smart connect with 30 second total timeout
    // This will try all saved networks in range
    if (smartConnect(30000)) {
      Serial.println("Smart reconnect successful!");
      wifiWasConnected = true;
    } else {
      Serial.println("Smart reconnect failed - will retry in 60 seconds");
      wifiWasConnected = false;
    }
  }
}

// Smart device WiFi connection: scan for networks and connect to the strongest saved network
bool WiFi_Client::smartConnect(uint32_t timeoutMs) {
  Serial.println("=== Starting Smart WiFi Connection ===");
  
  // CRITICAL FIX: Initialize WiFi mode before any operations
  // This fixes the race condition where WiFi isn't fully initialized at startup
  if (WiFi.getMode() == WIFI_MODE_NULL) {
    Serial.println("Initializing WiFi radio...");
    WiFi.mode(WIFI_STA);
    delay(100);  // Give WiFi stack time to initialize
  }
  
  // If no saved networks, nothing to connect to
  if (savedNetworks.size() == 0) {
    Serial.println("No saved networks available");
    return false;
  }
  
  Serial.printf("Found %d saved networks\n", savedNetworks.size());
  
  // Step 1: Scan for available networks
  Serial.println("Scanning for available networks...");
  int numNetworks = WiFi.scanNetworks();
  
  if (numNetworks <= 0) {
    Serial.println("No networks found in scan");
    WiFi.scanDelete();
    return false;
  }
  
  Serial.printf("Found %d networks in range\n", numNetworks);
  
  // Step 2: Build a list of available saved networks sorted by signal strength
  struct NetworkMatch {
    char ssid[33];
    char password[65];
    int rssi;
  };
  
  std::vector<NetworkMatch> matches;
  
  // Find all saved networks that are in range
  for (size_t i = 0; i < savedNetworks.size(); i++) {
    for (int j = 0; j < numNetworks; j++) {
      String scannedSsid = WiFi.SSID(j);
      if (strcmp(savedNetworks[i].ssid, scannedSsid.c_str()) == 0) {
        NetworkMatch match;
        strncpy(match.ssid, savedNetworks[i].ssid, sizeof(match.ssid) - 1);
        match.ssid[sizeof(match.ssid) - 1] = '\0';
        strncpy(match.password, savedNetworks[i].password, sizeof(match.password) - 1);
        match.password[sizeof(match.password) - 1] = '\0';
        match.rssi = WiFi.RSSI(j);
        matches.push_back(match);
        Serial.printf("  Found saved network: %s (RSSI: %d)\n", match.ssid, match.rssi);
        break;
      }
    }
  }
  
  WiFi.scanDelete();
  
  if (matches.size() == 0) {
    Serial.println("No saved networks found in range");
    return false;
  }
  
  // Sort by RSSI (strongest first)
  std::sort(matches.begin(), matches.end(), [](const NetworkMatch &a, const NetworkMatch &b) {
    return a.rssi > b.rssi;
  });
  
  Serial.printf("Attempting to connect to %d matching networks (sorted by strength)...\n", matches.size());
  
  // Step 3: Try each network in order (strongest first)
  unsigned long startTime = millis();
  
  for (size_t i = 0; i < matches.size(); i++) {
    // Check if we've exceeded total timeout
    if (millis() - startTime > timeoutMs) {
      Serial.println("Overall timeout reached");
      WiFi.disconnect();
      return false;
    }
    
    Serial.printf("Attempting #%d: %s (RSSI: %d)\n", i + 1, matches[i].ssid, matches[i].rssi);
    
    // CRITICAL FIX: Increase timeout to 7 seconds for startup connections
    // WiFi radio may not be fully initialized at power-on, needs more time
    WiFi.begin(matches[i].ssid, matches[i].password);
    
    unsigned long connectStart = millis();
    unsigned long connectTimeout = 7000;  // Increased from 3000 to 7000ms
    
    while (WiFi.status() != WL_CONNECTED) {
      if (millis() - connectStart > connectTimeout) {
        Serial.printf("  Timeout connecting to %s\n", matches[i].ssid);
        break;
      }
      delay(100);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("Successfully connected to %s!\n", matches[i].ssid);
      Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
      
      // Update current SSID/password
      setSsid(matches[i].ssid);
      setPassword(matches[i].password);
      
      // Track connection state for auto-reconnect
      wifiWasConnected = true;
      lastConnectAttempt = millis();
      
      // Call post-connection callback if set
      if (onConnectedCallback != nullptr) {
        Serial.println("Executing post-connection callback...");
        onConnectedCallback();
      }
      
      return true;
    }
  }
  
  Serial.println("Failed to connect to any saved network");
  WiFi.disconnect();
  wifiWasConnected = false;
  return false;
}

// Join a specific network with the given credentials
bool WiFi_Client::joinNetwork(const char *ssid, const char *password, uint32_t timeoutMs) {
  // Validate inputs
  if (ssid == nullptr || strlen(ssid) == 0) {
    Serial.println("joinNetwork: Invalid SSID (null or empty)");
    return false;
  }
  
  // Handle null password (open network)
  if (password == nullptr) {
    password = "";
  }
  
  Serial.printf("=== Attempting to join network: %s ===\n", ssid);
  
  // Disconnect from any current network
  if (WiFi.isConnected()) {
    Serial.println("Disconnecting from current network...");
    WiFi.disconnect();
    delay(100);
  }
  
  // Attempt to connect
  Serial.printf("Connecting to %s...\n", ssid);
  WiFi.begin(ssid, password);
  
  unsigned long startTime = millis();
  
  while (WiFi.status() != WL_CONNECTED) {
    if (millis() - startTime > timeoutMs) {
      Serial.printf("Timeout connecting to %s\n", ssid);
      WiFi.disconnect();
      return false;
    }
    lv_task_handler();
    delay(100);
  }
  
  // Successfully connected
  Serial.printf("Successfully connected to %s!\n", ssid);
  Serial.printf("IP address: %s\n", WiFi.localIP().toString().c_str());
  
  // Update current SSID/password
  setSsid(ssid);
  setPassword(password);
  
  // Save network if not already saved
  int idx = findNetworkIndex(ssid);
  if (idx >= 0) {
    // Update existing password
    strncpy(savedNetworks[idx].password, password, sizeof(savedNetworks[idx].password) - 1);
    savedNetworks[idx].password[sizeof(savedNetworks[idx].password) - 1] = '\0';
  } else {
    // Add new network
    WifiSavedNetwork newNetwork;
    strncpy(newNetwork.ssid, ssid, sizeof(newNetwork.ssid) - 1);
    newNetwork.ssid[sizeof(newNetwork.ssid) - 1] = '\0';
    strncpy(newNetwork.password, password, sizeof(newNetwork.password) - 1);
    newNetwork.password[sizeof(newNetwork.password) - 1] = '\0';
    savedNetworks.push_back(newNetwork);
  }
  
  // Save to storage
  extern SerializableConfigs serializableConfigs;
  serializableConfigs.write();
  
  // Call post-connection callback if set
  if (onConnectedCallback != nullptr) {
    Serial.println("Executing post-connection callback...");
    onConnectedCallback();
  }
  
  return true;
}

// Call this from the main loop to process WiFi scan results
void WiFi_Client::asyncScanUpdate() {
  if (!wifiScanInProgress) {
    return;
  }

  // If the screen changed since the scan started, do not touch UI.
  // Still clean up the scan when it completes/aborts.
  const bool stale = (wifiScanGeneration != wifiScreenGeneration);

  // Check for timeout
  if (millis() - wifiScanStartTime > WIFI_SCAN_TIMEOUT_MS) {
    Serial.println(">> WiFi scan timeout");
    WiFi.scanDelete();  // Clean up
    wifiScanInProgress = false;
    return;
  }

  // Check scan status
  int16_t result = WiFi.scanComplete();

  if (result == WIFI_SCAN_RUNNING) {
    // Still scanning, check again next loop iteration
    return;
  }

  if (result == WIFI_SCAN_FAILED) {
    Serial.println(">> WiFi scan failed");
    WiFi.scanDelete();  // Clean up any partial results
    wifiScanInProgress = false;
    return;
  }

  // Scan complete! result contains the number of networks found
  Serial.println(">> WiFi scan complete");
  Serial.printf(">> %d networks found\n", result);

  // If the user navigated away and is still off-screen, don't touch LVGL now.
  // We'll keep the latest scan result and apply it next time the WiFi screen loads.
  if (!wifiScreenActive) {
    Serial.println(">> WiFi scan finished while off-screen; caching result for next load");
    // We still package results below; just don't apply yet.
  } else if (stale) {
    // The WiFi screen was unloaded/reloaded during the scan. That's fine: we'll apply
    // to the current screen instance by using the current generation below.
    Serial.println(">> WiFi screen reloaded during scan; applying to current instance");
  }

  // Package scan results and apply on the LVGL thread.
  // Limit to a reasonable number of rows.
  int networkCount = result;
  if (networkCount > 20) networkCount = 20;

  if (networkCount > 0) {
    WifiUiScanResult *ui = (WifiUiScanResult *)lv_mem_alloc(sizeof(WifiUiScanResult));
    if (ui) {
      ui->gen = wifiScreenActive ? wifiScreenGeneration : wifiScanGeneration;
      ui->count = networkCount;

      for (int i = 0; i < networkCount; i++) {
        // CRITICAL FIX: WiFi.SSID(i) returns String - avoid by using direct pointer
        // Note: WiFi.SSID(i) returns a String, but we can get const char* from it temporarily
        String ssid_temp = WiFi.SSID(i);
        const char *ssidStr = ssid_temp.c_str();
        
        strncpy(ui->nets[i].ssid, ssidStr, sizeof(ui->nets[i].ssid) - 1);
        ui->nets[i].ssid[sizeof(ui->nets[i].ssid) - 1] = '\0';
        // ssid_temp destroyed here - String freed

        ui->nets[i].rssi = WiFi.RSSI(i);
        ui->nets[i].isOpen = (WiFi.encryptionType(i) == WIFI_AUTH_OPEN);
      }
      if (wifiScreenActive) {
        // Marshal to LVGL's thread; the handler re-checks generation + object validity.
        lv_async_call(applyWifiScanResultToUi, ui);
      } else {
        // Cache the most recent result (drop older one if present).
        if (pendingWifiUiResult) lv_mem_free(pendingWifiUiResult);
        pendingWifiUiResult = ui;
      }
    }
  } else {
    Serial.println(">> No networks found");
  }

  // Clean up scan results when done
  WiFi.scanDelete();
  wifiScanInProgress = false;
}

void WiFi_Client::serializeConfig(JsonDocument &doc) {
  Serial.printf("Writing %d wifi networks to storage\n", savedNetworks.size());
  JsonObject wifiDoc = doc["wifi_settings"].to<JsonObject>();
  JsonArray savedNetworksArr = wifiDoc["saved_networks"].to<JsonArray>();
  
  // CRITICAL FIX: Iterate through vector instead of unordered_map
  for (size_t i = 0; i < savedNetworks.size(); i++) {
    JsonObject savedNetworkObj = savedNetworksArr.add<JsonObject>();
    Serial.printf("  Saving %s / %s\n", savedNetworks[i].ssid, savedNetworks[i].password);
    savedNetworkObj["ssid"] = savedNetworks[i].ssid;
    savedNetworkObj["password"] = savedNetworks[i].password;
  }
}

void WiFi_Client::deserializeConfig(JsonDocument &doc) {
  Serial.println("Reading wifi settings from storage");
  
  // UPDATED: Use modern ArduinoJson API instead of deprecated containsKey()
  if (!doc["wifi_settings"].is<JsonObject>()) {
    Serial.println("No wifi_settings found in storage");
    return;
  }
  
  JsonObject wifiDoc = doc["wifi_settings"];
  
  // UPDATED: Use modern ArduinoJson API
  if (!wifiDoc["saved_networks"].is<JsonArray>()) {
    Serial.println("No saved_networks found in storage");
    return;
  }
  
  JsonArray savedNetworksArr = wifiDoc["saved_networks"];
  
  // CRITICAL FIX: Read into vector without String allocations
  savedNetworks.clear();  // Clear existing networks
  
  for (JsonObject savedNetworkObj : savedNetworksArr) {
    WifiSavedNetwork network;
    
    // Copy SSID and password from JSON (ArduinoJson handles const char* safely)
    const char *ssid = savedNetworkObj["ssid"];
    const char *password = savedNetworkObj["password"];
    
    if (ssid && password) {
      strncpy(network.ssid, ssid, sizeof(network.ssid) - 1);
      network.ssid[sizeof(network.ssid) - 1] = '\0';
      
      strncpy(network.password, password, sizeof(network.password) - 1);
      network.password[sizeof(network.password) - 1] = '\0';
      
      Serial.printf("Reading network from storage %s / %s\n", network.ssid, network.password);
      savedNetworks.push_back(network);
    }
  }
  
  Serial.printf("Loaded %d saved networks from storage\n", savedNetworks.size());
}