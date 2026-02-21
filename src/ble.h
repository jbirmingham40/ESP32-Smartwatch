#pragma once

#ifndef BLE_H
#define BLE_H

extern "C" {
#include "esp_heap_caps.h"
#include "sdkconfig.h"
}

#define nimble_malloc(size) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define nimble_free(ptr) heap_caps_free(ptr)

#include <Arduino.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "NimBLEDevice.h"

// ============================================================================
// AMS (Apple Media Service) Definitions
// ============================================================================

// AMS Remote Commands
enum AMSRemoteCommand : uint8_t {
  AMS_REMOTE_COMMAND_PLAY = 0,
  AMS_REMOTE_COMMAND_PAUSE = 1,
  AMS_REMOTE_COMMAND_TOGGLE_PLAY_PAUSE = 2,
  AMS_REMOTE_COMMAND_NEXT_TRACK = 3,
  AMS_REMOTE_COMMAND_PREVIOUS_TRACK = 4,
  AMS_REMOTE_COMMAND_VOLUME_UP = 5,
  AMS_REMOTE_COMMAND_VOLUME_DOWN = 6,
  AMS_REMOTE_COMMAND_ADVANCE_REPEAT_MODE = 7,
  AMS_REMOTE_COMMAND_ADVANCE_SHUFFLE_MODE = 8,
  AMS_REMOTE_COMMAND_SKIP_FORWARD = 9,
  AMS_REMOTE_COMMAND_SKIP_BACKWARD = 10,
  AMS_REMOTE_COMMAND_LIKE_TRACK = 11,
  AMS_REMOTE_COMMAND_DISLIKE_TRACK = 12,
  AMS_REMOTE_COMMAND_BOOKMARK_TRACK = 13
};

// AMS Entity IDs
enum AMSEntityID : uint8_t {
  AMS_ENTITY_PLAYER = 0,
  AMS_ENTITY_QUEUE = 1,
  AMS_ENTITY_TRACK = 2
};

// AMS Player Attributes
enum AMSPlayerAttribute : uint8_t {
  AMS_PLAYER_ATTRIBUTE_NAME = 0,
  AMS_PLAYER_ATTRIBUTE_PLAYBACK_INFO = 1,
  AMS_PLAYER_ATTRIBUTE_VOLUME = 2
};

// AMS Track Attributes
enum AMSTrackAttribute : uint8_t {
  AMS_TRACK_ATTRIBUTE_ARTIST = 0,
  AMS_TRACK_ATTRIBUTE_ALBUM = 1,
  AMS_TRACK_ATTRIBUTE_TITLE = 2,
  AMS_TRACK_ATTRIBUTE_DURATION = 3
};

// AMS Queue Attributes
enum AMSQueueAttribute : uint8_t {
  AMS_QUEUE_ATTRIBUTE_INDEX = 0,
  AMS_QUEUE_ATTRIBUTE_COUNT = 1,
  AMS_QUEUE_ATTRIBUTE_SHUFFLE_MODE = 2,
  AMS_QUEUE_ATTRIBUTE_REPEAT_MODE = 3
};

// AMS Playback States
enum AMSPlaybackState : uint8_t {
  AMS_PLAYBACK_STATE_PAUSED = 0,
  AMS_PLAYBACK_STATE_PLAYING = 1,
  AMS_PLAYBACK_STATE_REWINDING = 2,
  AMS_PLAYBACK_STATE_FAST_FORWARD = 3
};

// Structure to hold current media state
struct AMSMediaState {
  char trackTitle[128];
  char trackArtist[128];
  char trackAlbum[128];
  char trackDuration[16];
  char playerName[64];
  uint8_t playbackState;
  char volume[8];
  int queueIndex;
  int queueCount;
  uint8_t shuffleMode;
  uint8_t repeatMode;
  bool validTrackInfo;
  bool validPlayerInfo;
  bool validQueueInfo;
  float elapsedTime;  // Current elapsed time in seconds
  
  AMSMediaState() {
    memset(this, 0, sizeof(AMSMediaState));
  }
};

class BLE {

public:
  void begin();
  void run();
  void sendAction(uint32_t uid, bool isPositive);
  
  // AMS - Apple Media Service
  void sendMediaCommand(AMSRemoteCommand cmd);
  void requestTrackInfo();
  void requestPlayerInfo();
  void requestQueueInfo();
  void processAMSEntityUpdate();
  void updateMediaUIVariables();  // Call from main loop for smooth progress updates
  
  // SAFE getMediaState for Core 1 (UI) - returns read-only pointer to stable buffer
  const AMSMediaState* getMediaState() const {
    if (!displayBuffersPtr) {
      Serial.println(">> FATAL: displayBuffersPtr is NULL");
      static AMSMediaState emptyState;
      return &emptyState;
    }
    
    int idx = activeBufferIndex.load();
    
    // FIX: Validate index is in range
    if (idx != 0 && idx != 1) {
      Serial.printf(">> FATAL: Corrupted activeBufferIndex: %d\n", idx);
      return &displayBuffersPtr[0].data;
    }
    
    return &displayBuffersPtr[idx].data;
  }
  
  bool isAMSConnected() const { return amsConnected.load(); }
  
  // FIX: Flag to signal Core 1 that media UI needs updating
  // This replaces direct updateMediaUIVariables() calls from Core 0
  std::atomic<bool> mediaUIUpdateNeeded{false};

  // Flag set by Core 1 (action_refresh_media_info) to request a BLE refresh.
  // BLE_Task on Core 0 checks this and calls requestTrackInfo/requestPlayerInfo
  // from Core 0 only — prevents concurrent GATT operations across cores.
  std::atomic<bool> amsNeedsManualRefresh{false};

  class MyNimBLEServerCallbacks : public NimBLEServerCallbacks {
  public:
    MyNimBLEServerCallbacks(BLE* parent) : bleParent(parent) {}
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo);
    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason);
  private:
    BLE* bleParent;
  };

private:
  void dscNotifyCallback(uint8_t* pData, size_t length);
  void processNotificationData();
  void resetRemotePointers();
  void handleIncomingCall(const char* callerName, const char* callerNumber, uint32_t uid);
  
  // Store current call UID for accept/decline actions
  uint32_t incomingCallUid = 0;
  
  // UID of the "Active Call" notification iOS adds after a call is answered.
  // We track this UID and fire queueClearActiveCall() only when IT is removed —
  // the true end-of-call signal.
  uint32_t activeCallNotificationUid = 0;

  // Data buffer in PSRAM for reassembling multi-packet responses
  uint8_t* dataBuffer = nullptr;
  size_t dataBufferSize = 0;
  size_t dataBufferCapacity = 0;
  
  uint8_t* processingBuffer = nullptr;
  size_t processingBufferSize = 0;
  
  std::atomic<bool> dataReady{false};
  std::atomic<bool> subscribed{false};
  std::atomic<bool> waitingForResponse{false};
  std::atomic<bool> initialized{false};
  
  // Current notification event
  uint8_t nextEventUid[4];
  uint8_t nextEventCategory;
  uint8_t nextEventFlags;
  std::atomic<bool> nextEventActive{false};

  char lastCallName[128];
  char lastCallNumber[64];
  
  // UUIDs
  NimBLEUUID ancsServiceUUID = NimBLEUUID("7905F431-B5CE-4E99-A40F-4B1E122D00D0");
  NimBLEUUID nscUUID = NimBLEUUID("9FBF120D-6301-42D9-8C58-25E699A21DBD");
  NimBLEUUID cpUUID = NimBLEUUID("69D1D8F3-45E1-49A8-9821-9BBDFDAAD9D9");
  NimBLEUUID dscUUID = NimBLEUUID("22EAC6E9-24D6-4BB5-BE44-B36ACE7C7BFB");
  NimBLEUUID heartRateServiceUUID = NimBLEUUID((uint16_t)0x180D);
  NimBLEUUID heartRateMeasurementUUID = NimBLEUUID((uint16_t)0x2A37);

  NimBLEClient* pClient = nullptr;
  NimBLERemoteCharacteristic* pCPChar = nullptr;
  NimBLECharacteristic* pHeartRateChar = nullptr;
  
  // Mutex for protecting pointer access
  SemaphoreHandle_t blePointerMutex = nullptr;
  
  // FIX: Connection generation counter - incremented on every disconnect
  // Used to detect stale pointers after releasing the mutex
  std::atomic<uint32_t> connectionGeneration{0};
  
  // FIX: Track connection establishment time - no BLE I/O during stabilization period
  unsigned long connectionEstablishedTime = 0;
  static const unsigned long BLE_STABILIZATION_MS = 500;  // Wait after connect before I/O
  
  // FIX: Prevent concurrent BLE operations that can confuse NimBLE's response handlers
  std::atomic<bool> bleOperationInProgress{false};
  
  // ============================================================================
  // AMS (Apple Media Service)
  // ============================================================================
  
  // AMS UUIDs
  NimBLEUUID amsServiceUUID = NimBLEUUID("89D3502B-0F36-433A-8EF4-C502AD55F8DC");
  NimBLEUUID amsRemoteCommandUUID = NimBLEUUID("9B3C81D8-57B1-4A8A-B8DF-0E56F7CA51C2");
  NimBLEUUID amsEntityUpdateUUID = NimBLEUUID("2F7CABCE-808D-411F-9A0C-BB92BA96C102");
  NimBLEUUID amsEntityAttributeUUID = NimBLEUUID("C6B2F38C-23AB-46D8-A6AB-A3A870BBD5D7");
  
  // AMS Characteristics
  NimBLERemoteCharacteristic* pAMSRemoteCommand = nullptr;
  NimBLERemoteCharacteristic* pAMSEntityUpdate = nullptr;
  NimBLERemoteCharacteristic* pAMSEntityAttribute = nullptr;
  
  // AMS State - DOUBLE BUFFERING for Core Safety  
  // Core 0 maintains a single write buffer that accumulates all updates
  // Core 1 reads from one of two display buffers (active/inactive)
  // When ready, Core 0 copies write buffer -> inactive display buffer, then swaps
  
  // OPTIMIZATION: Buffers allocated in PSRAM instead of embedded in BSS (saves ~1.5KB internal heap)
  // Each buffer aligned to 64-byte cache lines to prevent false sharing between cores
  struct CacheAlignedBuffer {
    AMSMediaState data;
    uint8_t padding[16];
  };
  
  CacheAlignedBuffer* writeBufferPtr = nullptr;      // Single write buffer for Core 0
  CacheAlignedBuffer* displayBuffersPtr = nullptr;    // Array of 2 display buffers for Core 1
  
  std::atomic<int> activeBufferIndex{0};  // Which display buffer Core 1 reads from (0 or 1)
  
  std::atomic<bool> amsConnected{false};
  std::atomic<bool> amsNeedsInitialRequest{false};  // Flag to request info on next cycle
  std::atomic<bool> amsDiscoveryAttempted{false};   // Flag to ensure discovery only runs once
  std::atomic<bool> amsNeedsTrackRefresh{false};    // Flag for track change detection
  
  // Track change detection
  char previousTrackTitle[128] = {0};
  unsigned long lastTrackInfoRequest = 0;
  static const unsigned long TRACK_INFO_REFRESH_INTERVAL = 30000;  // 30 seconds
  
  // FIX: Consecutive read failure counter - auto-disconnect AMS after repeated failures
  int consecutiveReadFailures = 0;
  static const int MAX_CONSECUTIVE_READ_FAILURES = 6;  // 3 calls Ã— 2 attrs minimum
  
  // Helper methods for double-buffering
  // Core 0 always writes to writeBufferPtr, which accumulates all changes
  // Only when swapping do we copy writeBufferPtr -> inactive display buffer
  AMSMediaState* getWriteBuffer() {
    return &writeBufferPtr->data;  // Return pointer to the actual data
  }
  
  void swapBuffers() {
    // Copy accumulated changes from write buffer to inactive display buffer
    int inactiveIdx = 1 - activeBufferIndex.load();
    memcpy(&displayBuffersPtr[inactiveIdx].data, &writeBufferPtr->data, sizeof(AMSMediaState));
    
    // FIX: Memory barrier to ensure memcpy is complete and visible to other cores
    // ESP32 cache coherency: ensure all writes are flushed before atomic swap
    __asm__ __volatile__ ("memw" : : : "memory");
    
    // Atomically swap which buffer is active - Core 1 now reads our updates
    activeBufferIndex.store(inactiveIdx);
    
    // FIX: Another memory barrier to ensure the index change is visible
    __asm__ __volatile__ ("memw" : : : "memory");
  }
  
  // AMS Buffer for entity updates (in PSRAM)
  uint8_t* amsUpdateBuffer = nullptr;
  size_t amsUpdateBufferSize = 0;
  size_t amsUpdateBufferCapacity = 1024;
  std::atomic<bool> amsDataReady{false};
  
  // AMS Methods
  void amsEntityUpdateCallback(uint8_t* pData, size_t length);
  void parseAMSEntityUpdate(uint8_t* pData, size_t length);
  
  // Elapsed time interpolation anchors
  float elapsedTimeAnchor = 0.0f;        // iOS-reported elapsed time
  unsigned long elapsedTimeTimestamp = 0; // millis() when anchor was set
  
  // FIX: Authoritative BLE descriptor validity check using ble_gap_conn_find().
  // Must be called with a connHandle captured inside blePointerMutex.
  // Returns true only when the NimBLE host's connection table still contains
  // the descriptor for this handle, meaning BLE I/O will not crash.
  // See safeWriteValue / safeReadValue for usage.
  bool isDescriptorValid(uint16_t connHandle) const {
    if (connHandle == BLE_HS_CONN_HANDLE_NONE) return false;
    return ble_gap_conn_find(connHandle, nullptr) == 0;
  }
  
  // FIX: Helper to validate connection hasn't changed since we copied pointers
  // Returns true if the connection generation matches (connection is still valid)
  bool isConnectionValid(uint32_t savedGeneration) const {
    return connectionGeneration.load() == savedGeneration;
  }
  
  // FIX: Helper to check if connection is stable enough for BLE I/O
  // Prevents operations during the first 500ms after connection when NimBLE is settling
  bool isConnectionStable() const {
    if (!amsConnected.load()) return false;
    if (connectionEstablishedTime == 0) return false;
    return (::millis() - connectionEstablishedTime) > BLE_STABILIZATION_MS;
  }
  
  // FIX: Safe wrapper for BLE write operations
  // Returns false if connection dropped during the operation
  bool safeWriteValue(NimBLERemoteCharacteristic* pChar, uint8_t* data, size_t len, 
                      bool withResponse, uint32_t savedGen);
  bool safeReadValue(NimBLERemoteCharacteristic* pChar, NimBLEAttValue& outValue, 
                     uint32_t savedGen);
};

#endif