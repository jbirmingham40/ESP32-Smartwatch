#pragma once

#ifndef WIFI_CLIENT_H
#define WIFI_CLIENT_H

#include <vector>
#include "serializable_config.h"

// CRITICAL FIX: Replaced std::unordered_map<String, String> with vector of structs
// This eliminates ALL heap fragmentation from String allocations
typedef struct {
  char ssid[33];      // 32 + null terminator  
  char password[65];  // 64 + null terminator (WPA2 can be up to 63 chars)
} WifiSavedNetwork;

typedef struct {
  uint32_t gen;
  int count;
  struct {
    char ssid[33];  // 32 + NUL
    int rssi;
    bool isOpen;
  } nets[20];
} WifiUiScanResult;

struct ScreenOperation {
  uint8_t screenId;
  lv_scr_load_anim_t animType;
  uint16_t time;
  uint16_t delay;
  bool isPush;  // true = push, false = pop
};

class WiFi_Client : public SerializableConfig {
public:
  WiFi_Client() {}
  void startWifi();
  bool smartConnect(uint32_t timeoutMs = 10000, bool runPostConnectCallback = true);  // Smart device WiFi connection
  bool joinNetwork(const char *ssid, const char *password, uint32_t timeoutMs = 10000);  // Join specific network with timeout
  void setSsid(const char *ssid);
  const char *getSsid();
  void setPassword(const char *password);
  const char *getPassword();
  void asyncScanUpdate();
  // Call whenever WiFi is actively being used — resets the 30-second idle timer
  // and ensures a connection will be established if not already connected.
  void keepAlive();
  // Call from the main loop (Background_Tasks) to connect on demand and
  // disconnect automatically after 30 seconds of idle.
  void processLifecycle();
  
  // Callback to execute after successful WiFi connection
  void setOnConnectedCallback(void (*callback)()) {
    onConnectedCallback = callback;
  }
  
  // CRITICAL FIX: Changed API to avoid String allocations
  bool isNetworkSaved(const char *lookupSsid);
  bool isNetworkSaved(String lookupSsid);  // Keep for backward compatibility
  void removeSavedNetwork(const char *ssid);  // CHANGED: const char* instead of String
  const char *getPasswordForSavedSsid(const char *ssid);  // CHANGED: returns const char*, not String&
  bool isConnected();

public:
  void serializeConfig(JsonDocument &doc);
  void deserializeConfig(JsonDocument &doc);

private:
  char ssid[33] = {0};      // CHANGED: was 32, now 33 for null terminator
  char password[65] = {0};  // CHANGED: was 32, now 65 for WPA2 passwords

  // CRITICAL FIX: Replaced unordered_map with vector - NO heap allocations!
  std::vector<WifiSavedNetwork> savedNetworks;
  
  // Callback function to execute after successful connection
  void (*onConnectedCallback)() = nullptr;

  unsigned long wifiLastUsedMs = 0;  // millis() of last keepAlive() call
  bool wifiNeedsConnect = false;     // set by keepAlive(); cleared by processLifecycle()

  // Helper: Find network in savedNetworks vector
  int findNetworkIndex(const char *ssid);
};

#endif
