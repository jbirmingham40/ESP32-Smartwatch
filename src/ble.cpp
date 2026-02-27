#include <Arduino.h>
#include <esp_heap_caps.h>
#include "NimBLEDevice.h"
#include "ble.h"
#include "notifications.h"
#include "src/eez-flow.h"
#include "src/screens.h"
#include "src/vars.h"
#include "media_controls.h"

extern MediaControls mediaControls;

// Buffer sizes - allocated in PSRAM
#define DATA_BUFFER_CAPACITY 1024
#define PROCESSING_BUFFER_CAPACITY 1024

// CRITICAL: Macros for NULL pointer validation
// Prevents crashes from use-after-free or uninitialized pointers
#define VALIDATE_PTR(ptr, name) \
  do { \
    if (!(ptr) || (void*)(ptr) == (void*)0xFFFFFFFF) { \
      Serial.printf("ERROR: Invalid pointer '%s' at %s:%d\n", name, __FILE__, __LINE__); \
      return; \
    } \
  } while(0)

#define VALIDATE_PTR_RET(ptr, name, retval) \
  do { \
    if (!(ptr) || (void*)(ptr) == (void*)0xFFFFFFFF) { \
      Serial.printf("ERROR: Invalid pointer '%s' at %s:%d\n", name, __FILE__, __LINE__); \
      return retval; \
    } \
  } while(0)

// CRITICAL: Safe strlen that won't crash on corrupted memory
// Returns length up to maxLen, or 0 if string appears corrupted
static size_t safe_strlen(const char* str, size_t maxLen = 512) {
  if (!str || (void*)str == (void*)0xFFFFFFFF) {
    return 0;
  }
  
  for (size_t i = 0; i < maxLen; i++) {
    // Check if we're about to read from invalid memory
    // This catches poison value bytes in the string
    if ((uint8_t)str[i] == 0xFF && i > 0) {
      // Found 0xFF byte - likely corrupted
      // But allow it if it's actually part of valid UTF-8
      if (i + 1 < maxLen && (uint8_t)str[i+1] != 0xFF) {
        continue;  // Probably valid UTF-8
      } else {
        // Multiple 0xFF in a row = corrupted
        Serial.printf(">> WARNING: Corrupted string detected at offset %d\n", i);
        return i;  // Return length up to corruption
      }
    }
    
    if (str[i] == '\0') {
      return i;
    }
  }
  
  // No null terminator found within maxLen
  Serial.printf(">> WARNING: String not null-terminated within %d bytes\n", maxLen);
  return maxLen;
}

// Global instance
extern NotificationStore notificationStore;

void BLE::begin() {
  if (initialized.exchange(true)) {
    Serial.println(F(">> BLE::begin() already called, skipping"));
    return;
  }

  // Create mutex for pointer protection
  blePointerMutex = xSemaphoreCreateMutex();
  if (!blePointerMutex) {
    Serial.println(F(">> FATAL: Failed to create BLE mutex!"));
    initialized = false;
    return;
  }

  Serial.println(F("============================================"));
  Serial.println(F("ESP32 ANCS Notification Reader"));
  Serial.println(F("============================================"));
  
  // OPTIMIZATION: Allocate cache-aligned AMS buffers in PSRAM (saves ~1.5KB internal heap)
  writeBufferPtr = (CacheAlignedBuffer*)heap_caps_aligned_alloc(
      64, sizeof(CacheAlignedBuffer), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  displayBuffersPtr = (CacheAlignedBuffer*)heap_caps_aligned_alloc(
      64, sizeof(CacheAlignedBuffer) * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  
  if (!writeBufferPtr || !displayBuffersPtr) {
    Serial.println(F(">> FATAL: Failed to allocate AMS buffers in PSRAM!"));
    initialized = false;
    return;
  }
  
  // FIX: Initialize and validate the cache-aligned buffers
  memset(&writeBufferPtr->data, 0, sizeof(AMSMediaState));
  memset(&displayBuffersPtr[0].data, 0, sizeof(AMSMediaState));
  memset(&displayBuffersPtr[1].data, 0, sizeof(AMSMediaState));
  activeBufferIndex.store(0);
  
  // Validate alignment
  Serial.printf(">> Buffer alignment check:\n");
  Serial.printf("   writeBuffer at: 0x%08x (align: %d)\n", 
                (uint32_t)writeBufferPtr, (uint32_t)writeBufferPtr % 64);
  Serial.printf("   displayBuffers[0] at: 0x%08x (align: %d)\n", 
                (uint32_t)&displayBuffersPtr[0], (uint32_t)&displayBuffersPtr[0] % 64);
  Serial.printf("   displayBuffers[1] at: 0x%08x (align: %d)\n", 
                (uint32_t)&displayBuffersPtr[1], (uint32_t)&displayBuffersPtr[1] % 64);

  // Allocate buffers in PSRAM
  dataBuffer = (uint8_t*)heap_caps_malloc(DATA_BUFFER_CAPACITY, MALLOC_CAP_SPIRAM);
  processingBuffer = (uint8_t*)heap_caps_malloc(PROCESSING_BUFFER_CAPACITY, MALLOC_CAP_SPIRAM);
  amsUpdateBuffer = (uint8_t*)heap_caps_malloc(amsUpdateBufferCapacity, MALLOC_CAP_SPIRAM);

  if (!dataBuffer || !processingBuffer || !amsUpdateBuffer) {
    Serial.println(F(">> FATAL: Failed to allocate BLE buffers in PSRAM!"));
    initialized = false;
    return;
  }

  dataBufferSize = 0;
  dataBufferCapacity = DATA_BUFFER_CAPACITY;
  processingBufferSize = 0;

  NimBLEDevice::init("ESP32-SMARTWATCH");
  NimBLEDevice::setMTU(256);
  NimBLEDevice::setSecurityAuth(true, true, true);  // bonding, mitm, sc
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_DISPLAY_YESNO);
  NimBLEDevice::setPower(ESP_PWR_LVL_N0);  // 0 dBm — plenty for a wrist-worn device

  NimBLEServer* pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyNimBLEServerCallbacks(this));

  // Heart Rate Service (for iOS visibility)  
  NimBLEService* pHeartRateService = pServer->createService(heartRateServiceUUID);
  pHeartRateChar = pHeartRateService->createCharacteristic(
    heartRateMeasurementUUID,
    NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY | 
    NIMBLE_PROPERTY::READ_AUTHEN);  // Require pairing to read
  
  uint8_t heartRateValue[2] = { 0, 75 };
  pHeartRateChar->setValue(heartRateValue, 2);
  pHeartRateService->start();

  // Device Info Service
  NimBLEService* pDeviceInfoService = pServer->createService("180A");
  NimBLECharacteristic* pManufacturerChar = pDeviceInfoService->createCharacteristic(
    "2A29", NIMBLE_PROPERTY::READ);
  pManufacturerChar->setValue("ESP32");
  pDeviceInfoService->start();

  // Advertising - MINIMAL approach for iOS pairing
  NimBLEAdvertising* pAdv = NimBLEDevice::getAdvertising();
  
  // Advertisement Data - Keep it MINIMAL
  NimBLEAdvertisementData advData;
  advData.setFlags(0x06);  // BR/EDR Not Supported, LE General Discoverable
  advData.setCompleteServices(heartRateServiceUUID);  // Heart Rate Service ONLY

  // ANCS Service Solicitation
  uint8_t ancsSol[18] = { 17, 0x15 };
  memcpy(&ancsSol[2], ancsServiceUUID.getValue(), 16);
  advData.addData(ancsSol, 18);

  pAdv->setAdvertisementData(advData);
  
  // Scan Response - Just the name
  NimBLEAdvertisementData scanData;
  scanData.setName("ESP32-SMARTWATCH");
  pAdv->setScanResponseData(scanData);

  // Slower advertising interval saves ~5 mA vs default ~100 ms.
  // 800 * 0.625 ms = 500 ms min, 1600 * 0.625 ms = 1000 ms max.
  // Phone still pairs fine; discovery just takes a couple of seconds longer.
  pAdv->setMinInterval(800);
  pAdv->setMaxInterval(1600);

  pAdv->start();
}

void BLE::run() {
  // CRITICAL: Validate BLE object itself
  VALIDATE_PTR(this, "BLE object in run");
  
  static unsigned long requestTime = 0;
  unsigned long now = millis();

  // Early exit if not properly initialized
  if (!dataBuffer || !processingBuffer) {
    return;
  }

  // Timeout
  if (waitingForResponse.load() && requestTime > 0 && (now - requestTime) > 3000) {    
    waitingForResponse = false;
    requestTime = 0;
    dataBufferSize = 0;
    dataReady = false;
  }

  // Process pending data
  if (dataReady.load()) {
    Serial.println(">> Processing notification data...");
    processNotificationData();
    dataReady = false;
    waitingForResponse = false;
    requestTime = 0;
  }

  // Connection check with mutex protection
  bool isConnected = false;
  if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
    isConnected = (pClient != nullptr && pClient->isConnected());
    xSemaphoreGive(blePointerMutex);
  }

  if (!isConnected) {
    if (subscribed.load()) {
      Serial.println(">> Lost connection");
      
      // CRITICAL: Manually reset AMS state since onDisconnect callback might not fire
      if (blePointerMutex && xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        pAMSRemoteCommand = nullptr;
        pAMSEntityUpdate = nullptr;
        pAMSEntityAttribute = nullptr;
        xSemaphoreGive(blePointerMutex);
      }
      amsConnected = false;
      
      // FIX: Clear ALL buffers on disconnect so UI always sees empty state
      if (writeBufferPtr && displayBuffersPtr) {
        memset(&writeBufferPtr->data, 0, sizeof(AMSMediaState));
        memset(&displayBuffersPtr[0].data, 0, sizeof(AMSMediaState));
        memset(&displayBuffersPtr[1].data, 0, sizeof(AMSMediaState));
      }
      
      amsNeedsInitialRequest = false;
      amsNeedsTrackRefresh = false;
      amsDiscoveryAttempted = false;
      amsDataReady = false;
      connectionGeneration++;  // FIX: Invalidate any outstanding local pointer copies
      Serial.println(">> AMS state reset due to disconnect");
    }
    subscribed = false;
    waitingForResponse = false;
    nextEventActive = false;
    requestTime = 0;
    return;
  }

  try {
    // Subscribe if encrypted - Check connection state with mutex protection
    NimBLEClient* localClient = nullptr;
    bool shouldSubscribe = false;
    
    if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
      if (pClient && pClient->isConnected()) {
        bool isEncrypted = false;
        try {
          isEncrypted = pClient->getConnInfo().isEncrypted();
        } catch (...) {
          Serial.println(F(">> Exception in getConnInfo - connection may be stale"));
        }
        bool alreadySubscribed = subscribed.load();       
        if (isEncrypted && !alreadySubscribed) {
          localClient = pClient;  // Copy pointer for use outside mutex
          shouldSubscribe = true;
        }
      }
      xSemaphoreGive(blePointerMutex);
    }
    
    if (shouldSubscribe && localClient) {
      Serial.println(F(">> Starting service discovery..."));

      // Call long-running operation WITHOUT holding mutex
      if (!localClient->discoverAttributes()) {
        Serial.println(F(">> Discovery failed"));
        return;
      }

      Serial.println(F(">> Discovery complete"));
      vTaskDelay(pdMS_TO_TICKS(100));

      // Step 2: Verify connection and get service (with mutex protection)
      NimBLERemoteService* pService = nullptr;
      if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (pClient == localClient && pClient && pClient->isConnected()) {
          try {
            pService = localClient->getService(ancsServiceUUID);
          } catch (...) {
            Serial.println(F(">> Exception getting ANCS service"));
            pService = nullptr;
          }
        } else {
          Serial.println(F(">> Connection lost after discovery"));
        }
        xSemaphoreGive(blePointerMutex);
      } else {
        Serial.println(F(">> Could not verify connection after discovery"));
        return;
      }

      if (!pService) {
        Serial.println(F(">> ANCS service not found"));
        return;
      }

      // Step 3: Verify connection and get characteristics (with mutex protection)
      NimBLERemoteCharacteristic* pNSC = nullptr;
      NimBLERemoteCharacteristic* pDSC = nullptr;
      NimBLERemoteCharacteristic* pCP = nullptr;
      
      if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (pClient == localClient && pClient && pClient->isConnected() && pService) {
          try {
            pNSC = pService->getCharacteristic(nscUUID);
            pDSC = pService->getCharacteristic(dscUUID);
            pCP = pService->getCharacteristic(cpUUID);
          } catch (...) {
            Serial.println(F(">> Exception getting characteristics"));
            pNSC = nullptr;
            pDSC = nullptr;
            pCP = nullptr;
          }
        } else {
          Serial.println(F(">> Connection lost before getting characteristics"));
        }
        xSemaphoreGive(blePointerMutex);
      } else {
        Serial.println(F(">> Could not verify connection for characteristics"));
        return;
      }

      if (!pNSC || !pDSC || !pCP) {
        Serial.println(F(">> Missing ANCS characteristics"));
        return;
      }

      BLE* self = this;

      // NSC callback
      auto nscCallback = [self](NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
        if (!self || !self->blePointerMutex) return;
        if (!pData || length < 8) return;
        
        // Take mutex before checking pClient
        if (xSemaphoreTake(self->blePointerMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
        
        bool clientValid = (self->pClient && self->pClient->isConnected());
        xSemaphoreGive(self->blePointerMutex);
        
        if (!clientValid) return;

        uint8_t eventID = pData[0];
        uint8_t eventFlags = pData[1];
        uint8_t categoryId = pData[2];
        uint32_t uid;
        memcpy(&uid, &pData[4], 4);

        if (eventID == 0 || eventID == 1) {
          if (!self->waitingForResponse.load()) {
            memcpy(self->nextEventUid, &pData[4], 4);
            self->nextEventCategory = categoryId;
            self->nextEventFlags = eventFlags;
            self->nextEventActive = true;
          }
        } else if (eventID == 2) {
          // FIX: incomingCallUid defaults to 0. iOS sometimes sends removal events with
          // UID 0 on reconnect. Without this guard, 0==0 spuriously triggers the missed-call
          // path before any real call was registered, popping the call screen and showing a
          // bogus "Missed Call" quick notification.
          if (self->incomingCallUid != 0 && uid == self->incomingCallUid) {
            // iOS removed the ringing notification. Two cases:
            // a) ACCEPTED (callInProgress==true): screen stays visible, no missed-call
            // b) MISSED/DECLINED (callInProgress==false): show missed-call, pop screen
            if (!notificationStore.isCallInProgress()) {
              static char s_missedCallMsg[256];
              static char s_missedCallIcon[32] = "phone";
              
              if (safe_strlen(self->lastCallNumber, sizeof(self->lastCallNumber)) > 0) {
                snprintf(s_missedCallMsg, sizeof(s_missedCallMsg), 
                        "Missed call from %s", self->lastCallNumber);
              } else {
                snprintf(s_missedCallMsg, sizeof(s_missedCallMsg), 
                        "Missed call");
              }
              
              notificationStore.queueAddNotification(
                "Phone",
                "Missed Call",
                self->lastCallName,
                s_missedCallMsg,
                "",
                uid,
                0,
                true,
                false,
                false,
                "",
                ""
              );
              
              notificationStore.queueQuickNotification(
                s_missedCallIcon,
                "Missed Call",
                self->lastCallName,
                s_missedCallMsg,
                true
              );
              
              notificationStore.queuePopCallScreen();
            } else {
              Serial.printf(">> Ringing UID %u removed (call accepted, screen stays visible)\n", uid);
            }
            
            self->incomingCallUid = 0;

          } else if (uid == self->activeCallNotificationUid) {
            // "Active Call" notification removed — call truly ended
            Serial.printf(">> Active Call UID %u removed — call ended\n", uid);
            self->activeCallNotificationUid = 0;
            notificationStore.queueClearActiveCall();
            notificationStore.queueRemove(uid);

          } else {
            notificationStore.queueRemove(uid);
          }
        }
      };

      // DSC callback
      auto dscCallback = [self](NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
        if (!self || !self->blePointerMutex) return;
        if (!pData || length == 0) return;
        
        if (xSemaphoreTake(self->blePointerMutex, pdMS_TO_TICKS(10)) != pdTRUE) return;
        
        bool clientValid = (self->pClient && self->pClient->isConnected());
        xSemaphoreGive(self->blePointerMutex);
        
        if (!clientValid) return;
        
        self->dscNotifyCallback(pData, length);
      };

      // Step 4: Verify connection and capture generation + handle before subscribing.
      //
      // FIX: This block previously only checked isConnected() (a cached NimBLE flag)
      // and then released the mutex before calling pNSC->subscribe()/pDSC->subscribe().
      // If the phone disconnects in that window, NimBLE frees the connection descriptor
      // while subscribe() is still blocked, causing:
      //   LoadProhibited at EXCVADDR 0x00000043 (offset 0x43 into freed ble_hs_conn).
      // Fix: capture ancsGen and ancsConnHandle inside the mutex, then call
      // isDescriptorValid(ancsConnHandle) immediately before each subscribe() call.
      uint32_t ancsGen = connectionGeneration.load();
      uint16_t ancsConnHandle = BLE_HS_CONN_HANDLE_NONE;

      if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool stillConnected = (pClient == localClient && pClient && pClient->isConnected());
        if (stillConnected) {
          ancsConnHandle = pClient->getConnHandle();
        }
        xSemaphoreGive(blePointerMutex);
        if (!stillConnected) {
          Serial.println(F(">> Connection lost before subscription"));
          return;
        }
      } else {
        Serial.println(F(">> Could not verify connection for subscription"));
        return;
      }

      try {
        if (!pNSC) {
          Serial.println(F(">> NSC characteristic is null"));
          return;
        }
        // FIX: ble_gap_conn_find() guard before NSC subscribe.
        Serial.println(F(">> [BLE-CRUMB] About to subscribe NSC"));
        if (!isConnectionValid(ancsGen) || !isDescriptorValid(ancsConnHandle)) {
          Serial.println(F(">> BLE descriptor freed before NSC subscribe - aborting"));
          return;
        }
        if (!pNSC->subscribe(true, nscCallback)) {
          Serial.println(F(">> NSC subscribe failed"));
          return;
        }
        Serial.println(F(">> [BLE-CRUMB] NSC subscribed OK"));
      } catch (...) {
        Serial.println(F(">> Exception subscribing to NSC"));
        return;
      }

      try {
        if (!pDSC) {
          Serial.println(F(">> DSC characteristic is null"));
          return;
        }
        // FIX: Same ble_gap_conn_find() guard before DSC subscribe.
        Serial.println(F(">> [BLE-CRUMB] About to subscribe DSC"));
        if (!isConnectionValid(ancsGen) || !isDescriptorValid(ancsConnHandle)) {
          Serial.println(F(">> BLE descriptor freed before DSC subscribe - aborting"));
          return;
        }
        if (!pDSC->subscribe(true, dscCallback)) {
          Serial.println(F(">> DSC subscribe failed"));
          return;
        }
        Serial.println(F(">> [BLE-CRUMB] DSC subscribed OK"));
      } catch (...) {
        Serial.println(F(">> Exception subscribing to DSC"));
        return;
      }

      // Final verification and commit
      if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (pClient == localClient && pClient && pClient->isConnected() && pCP) {
          pCPChar = pCP;
          subscribed = true;
          Serial.println(F(">> ANCS Subscribed and Ready"));
        } else {
          Serial.println(F(">> Connection lost or CP invalid before subscription complete"));
        }
        xSemaphoreGive(blePointerMutex);
      }
      
      // ========================================================================
      // AMS (Apple Media Service) Discovery - ONLY RUN ONCE
      // ========================================================================
      if (subscribed.load() && !amsDiscoveryAttempted.load()) {
        amsDiscoveryAttempted = true;
        Serial.println(F(">> Discovering AMS service..."));
        
        NimBLERemoteService* pAMSService = nullptr;
        if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
          if (pClient == localClient && pClient && pClient->isConnected()) {
            try {
              pAMSService = localClient->getService(amsServiceUUID);
            } catch (...) {
              Serial.println(F(">> Exception getting AMS service"));
              pAMSService = nullptr;
            }
          }
          xSemaphoreGive(blePointerMutex);
        }
        
        if (pAMSService) {
          Serial.println(F(">> AMS service found!"));
          
          NimBLERemoteCharacteristic* pRemoteCmd = nullptr;
          NimBLERemoteCharacteristic* pEntityUpdate = nullptr;
          NimBLERemoteCharacteristic* pEntityAttr = nullptr;
          
          // FIX: Capture connection generation BEFORE releasing the mutex below.
          // savedGen is used by safeWriteValue to detect disconnection between
          // the mutex release and the actual BLE I/O (the window where raw
          // writeValue() crashes with LoadProhibited at EXCVADDR 0x00000043).
          uint32_t savedGen = connectionGeneration.load();

          if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (pClient == localClient && pClient && pClient->isConnected() && pAMSService) {
              try {
                pRemoteCmd = pAMSService->getCharacteristic(amsRemoteCommandUUID);
                pEntityUpdate = pAMSService->getCharacteristic(amsEntityUpdateUUID);
                pEntityAttr = pAMSService->getCharacteristic(amsEntityAttributeUUID);
              } catch (...) {
                Serial.println(F(">> Exception getting AMS characteristics"));
                pRemoteCmd = nullptr;
                pEntityUpdate = nullptr;
                pEntityAttr = nullptr;
              }
            }
            xSemaphoreGive(blePointerMutex);
          }
          
          if (pRemoteCmd && pEntityUpdate && pEntityAttr) {
            
            BLE* self = this;
            auto amsCallback = [self](NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
              static std::atomic<uint32_t> callbackCount{0};
              callbackCount++;
                            
              if (!self || !self->blePointerMutex) {
                Serial.println(F(">> AMS callback: Invalid self or mutex"));
                return;
              }
              if (!pData || length == 0) {
                Serial.println(F(">> AMS callback: Invalid data"));
                return;
              }
                            
              self->amsEntityUpdateCallback(pData, length);
            };
            
            try {
              // FIX: Guard subscribe() with BOTH isConnectionValid AND isDescriptorValid.
              // isConnectionValid alone misses the window between ble_hs_conn being freed
              // and connectionGeneration being incremented in resetRemotePointers().
              // isDescriptorValid (ble_gap_conn_find) catches that window.
              if (!isConnectionValid(savedGen) || !isDescriptorValid(ancsConnHandle)) {
                Serial.println(F(">> AMS: descriptor freed before EntityUpdate subscribe — aborting"));
                return;
              }

              Serial.println(F(">> [BLE-CRUMB] About to subscribe AMS EntityUpdate"));
              if (pEntityUpdate->subscribe(true, amsCallback)) {
                Serial.println(F(">> Subscribed to AMS entity updates"));
                
                // FIX: CCCD readValue() removed entirely.
                // The original code read the CCCD descriptor and stored the value
                // in cccdVal which was never read again — dead diagnostic code.
                // More critically, NimBLERemoteDescriptor::readValue() dereferences
                // the same connection descriptor as writeValue() and crashes
                // identically (EXCVADDR 0x00000043) when the phone disconnects
                // between subscribe() and this call. There is no safeReadValue()
                // wrapper for descriptors, and since cccdVal was never used, the
                // correct fix is to delete this block.

                // FIX: Guard RemoteCmd subscribe with both validity checks (same as EntityUpdate).
                if (!isConnectionValid(savedGen) || !isDescriptorValid(ancsConnHandle)) {
                  Serial.println(F(">> AMS: descriptor freed before RemoteCmd subscribe — aborting"));
                  return;
                }
                if (pRemoteCmd && pRemoteCmd->canNotify()) {
                  auto remoteCmdCallback = [self](NimBLERemoteCharacteristic* pChar, uint8_t* pData, size_t length, bool isNotify) {
                  };
                  
                  Serial.println(F(">> [BLE-CRUMB] About to subscribe AMS RemoteCmd"));
                  if (pRemoteCmd->subscribe(true, remoteCmdCallback)) {
                    Serial.println(F(">> Subscribed to Remote Command notifications"));
                  } else {
                    Serial.println(F(">> Failed to subscribe to Remote Command"));
                  }
                }
                
                // Subscribe to Track entity updates
                uint8_t trackSubscribe[] = {
                  AMS_ENTITY_TRACK,
                  AMS_TRACK_ATTRIBUTE_ARTIST,
                  AMS_TRACK_ATTRIBUTE_ALBUM,
                  AMS_TRACK_ATTRIBUTE_TITLE,
                  AMS_TRACK_ATTRIBUTE_DURATION
                };
                
                // FIX: Replace raw writeValue() with safeWriteValue().
                // The mutex was released at line ~522 above. Between that release
                // and here, the phone can disconnect (screen lock, BLE timeout),
                // causing NimBLE to free the connection descriptor. A raw
                // writeValue() then dereferences offset 0x43 into freed memory →
                // LoadProhibited crash. safeWriteValue() calls ble_gap_conn_find()
                // to verify the descriptor is still live before any I/O attempt.
                bool subSuccess = safeWriteValue(pEntityUpdate, trackSubscribe, sizeof(trackSubscribe), true, savedGen);
                Serial.printf(">> Track entity subscription: %s\n", subSuccess ? "SUCCESS" : "FAILED");
                
                vTaskDelay(pdMS_TO_TICKS(100));

                // FIX: Abort early if connection dropped during the track subscribe
                // delay — avoids hitting the player write with a stale descriptor.
                if (!isConnectionValid(savedGen)) {
                  Serial.println(F(">> AMS discovery: connection lost after track subscribe"));
                  return;
                }
                
                // Subscribe to Player entity updates
                uint8_t playerSubscribe[] = {
                  AMS_ENTITY_PLAYER,
                  AMS_PLAYER_ATTRIBUTE_NAME,
                  AMS_PLAYER_ATTRIBUTE_PLAYBACK_INFO
                };
                
                // FIX: Same safeWriteValue guard as track subscribe above.
                bool playerSuccess = safeWriteValue(pEntityUpdate, playerSubscribe, sizeof(playerSubscribe), true, savedGen);
                Serial.printf(">> Player entity subscription: %s\n", playerSuccess ? "SUCCESS" : "FAILED");
                
                vTaskDelay(pdMS_TO_TICKS(100));
                
                // Store characteristics and mark as connected
                if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                  if (pClient == localClient && pClient && pClient->isConnected()) {
                    pAMSRemoteCommand = pRemoteCmd;
                    pAMSEntityUpdate = pEntityUpdate;
                    pAMSEntityAttribute = pEntityAttr;
                    amsConnected = true;
                    amsNeedsInitialRequest = true;
                    connectionEstablishedTime = millis();  // FIX: Track when connection stabilized
                    Serial.println(F(">> AMS fully connected!"));
                  }
                  xSemaphoreGive(blePointerMutex);
                }              
                
              } else {
                Serial.println(F(">> Failed to subscribe to AMS entity updates"));
              }
            } catch (...) {
              Serial.println(F(">> Exception subscribing to AMS"));
            }
          } else {
            Serial.println(F(">> Missing AMS characteristics"));
          }
        } else {
          Serial.println(F(">> AMS service not found (device may not support it)"));
        }
      }
    }

    // Request data - with mutex protection
    if (subscribed.load() && nextEventActive.load() && !waitingForResponse.load()) {
      if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        bool canWrite = (pCPChar != nullptr && pClient != nullptr && pClient->isConnected());
        NimBLERemoteCharacteristic* localCP = pCPChar;
        
        if (canWrite) {
          nextEventActive = false;
          waitingForResponse = true;
          requestTime = millis();

          uint8_t req[] = {
            0x00,  // GetNotificationAttributes
            nextEventUid[0], nextEventUid[1], nextEventUid[2], nextEventUid[3],
            0x00, 0xFF, 0xFF,  // AppIdentifier
            0x01, 0xFF, 0xFF,  // Title
            0x02, 0xFF, 0xFF,  // Subtitle
            0x03, 0xFF, 0xFF,  // Message
            0x04,              // MessageSize
            0x05,              // Date
            0x06,              // PositiveActionLabel
            0x07               // NegativeActionLabel
          };

          // FIX: Release mutex before write to prevent deadlock with callbacks
          xSemaphoreGive(blePointerMutex);
          
          // FIX: Use safe wrapper - ANCS control point needs write-with-response
          uint32_t savedGen = connectionGeneration.load();
          if (!safeWriteValue(localCP, req, sizeof(req), true, savedGen)) {
            Serial.println(">> Write failed");
            waitingForResponse = false;
            requestTime = 0;
          }
        } else {
          xSemaphoreGive(blePointerMutex);
        }
      }
    }

    // ========================================================================
    // AMS Initial Track Info Request (safe, outside mutex)
    // ========================================================================
    if (amsNeedsInitialRequest.load() && amsConnected.load()) {
      amsNeedsInitialRequest = false;
      Serial.println(F(">> [BLE-CRUMB] Firing initial requestTrackInfo"));
      requestTrackInfo();
      requestPlayerInfo();
      lastTrackInfoRequest = now;
    }
    
    // ========================================================================
    // AMS Track Change Refresh
    // ========================================================================
    if (amsNeedsTrackRefresh.load() && amsConnected.load()) {
      amsNeedsTrackRefresh = false;
      vTaskDelay(pdMS_TO_TICKS(200));
      requestTrackInfo();
      vTaskDelay(pdMS_TO_TICKS(100));
      requestPlayerInfo();
      lastTrackInfoRequest = now;
    }
    
    // ========================================================================
    // AMS Manual Refresh (requested from Core 1 via amsNeedsManualRefresh flag)
    // action_refresh_media_info() sets this flag instead of calling BLE I/O
    // from Core 1 directly — prevents concurrent GATT operations across cores.
    // ========================================================================
    if (amsNeedsManualRefresh.load() && amsConnected.load()) {
      amsNeedsManualRefresh = false;
      Serial.println(F(">> [AMS] Manual refresh requested from UI"));
      requestTrackInfo();
      vTaskDelay(pdMS_TO_TICKS(100));
      requestPlayerInfo();
      lastTrackInfoRequest = now;
      // Force artwork even if title/artist match what UI already knows.
      // Without this, a manual refresh for the same song (e.g. to retry a
      // failed artwork load) would not trigger update_album_artwork() because
      // updateMediaUIVariables() only fires it when titleOrArtistChanged.
      amsForceArtworkRefresh.store(true, std::memory_order_relaxed);
    }

    // ========================================================================
    // AMS Periodic Track Info Refresh - REMOVED
    // Entity Update notifications (subscribed at connection time) push all
    // track/player changes to us automatically. The periodic EA read/write
    // polling was redundant and caused LoadProhibited crashes when the phone
    // went to sleep (NimBLE reports isConnected==true but characteristic
    // internals are already freed — hardware fault, not catchable by try/catch).
    // ========================================================================

    vTaskDelay(pdMS_TO_TICKS(10));
  } catch (const std::exception& e) {
    Serial.printf(">> Exception in run(): %s\n", e.what());
    subscribed = false;
    waitingForResponse = false;
    nextEventActive = false;
  } catch (...) {
    Serial.println(F(">> Unknown exception in run()"));
    subscribed = false;
    waitingForResponse = false;
    nextEventActive = false;
  }
}

void BLE::dscNotifyCallback(uint8_t* pData, size_t length) {
  if (!pData || length == 0 || !dataBuffer) {
    return;
  }
  
  if (dataBufferSize + length > dataBufferCapacity) {
    dataBufferSize = 0;
    return;
  }

  memcpy(dataBuffer + dataBufferSize, pData, length);
  dataBufferSize += length;

  if (dataBufferSize < 5) return;
  if (dataBuffer[0] != 0) {
    Serial.printf(">> DSC: Invalid cmdID=%d\n", dataBuffer[0]);
    dataBufferSize = 0;
    return;
  }

  size_t pos = 5;
  bool complete = true;
  int attrCount = 0;

  while (pos + 3 <= dataBufferSize) {
    uint16_t attrLen = dataBuffer[pos + 1] | (dataBuffer[pos + 2] << 8);
    if (attrLen > 512) {
      Serial.printf(">> DSC: Bad attr length %d\n", attrLen);
      dataBufferSize = 0;
      return;
    }
    pos += 3;
    if (pos + attrLen > dataBufferSize) {
      complete = false;
      break;
    }
    pos += attrLen;
    attrCount++;
  }

  if (complete && attrCount > 0) {
    if (!dataReady.load()) {
      if (dataBufferSize > PROCESSING_BUFFER_CAPACITY) {
        Serial.printf(">> ERROR: dataBufferSize=%d exceeds capacity\n", dataBufferSize);
        dataBufferSize = 0;
        return;
      }
      
      VALIDATE_PTR(processingBuffer, "processingBuffer");
      VALIDATE_PTR(dataBuffer, "dataBuffer");
      
      memcpy(processingBuffer, dataBuffer, dataBufferSize);
      processingBufferSize = dataBufferSize;
      dataBufferSize = 0;
      dataReady = true;
    } else {
      dataBufferSize = 0;
    }
  }
}

void BLE::processNotificationData() {
  VALIDATE_PTR(processingBuffer, "processingBuffer");
  
  if (processingBufferSize < 5) {
    Serial.println(">> ERROR: processingBufferSize too small");
    processingBufferSize = 0;
    return;
  }
  
  if (processingBufferSize > PROCESSING_BUFFER_CAPACITY) {
    Serial.printf(">> ERROR: processingBufferSize=%d exceeds capacity\n", processingBufferSize);
    processingBufferSize = 0;
    return;
  }
  
  if (processingBuffer[0] != 0) {
    Serial.printf(">> ERROR: Invalid cmdID=%d\n", processingBuffer[0]);
    processingBufferSize = 0;
    return;
  }

  char appId[128] = { 0 };
  char title[128] = { 0 };
  char subtitle[128] = { 0 };
  char message[256] = { 0 };
  char dateTime[32] = { 0 };
  char positiveAction[32] = { 0 };
  char negativeAction[32] = { 0 };

  uint32_t uid;
  memcpy(&uid, processingBuffer + 1, 4);

  size_t pos = 5;
  while (pos + 3 <= processingBufferSize) {
    uint8_t attrID = processingBuffer[pos];
    uint16_t attrLen = processingBuffer[pos + 1] | (processingBuffer[pos + 2] << 8);
    
    if (attrLen > 1024) {
      Serial.printf(">> ERROR: Corrupt attrLen=%d at pos=%d, aborting parse\n", attrLen, pos);
      processingBufferSize = 0;
      return;
    }
    
    pos += 3;

    if (pos + attrLen > processingBufferSize) {
      Serial.printf(">> ERROR: attrLen=%d exceeds buffer at pos=%d\n", attrLen, pos);
      break;
    }

    char* dest = nullptr;
    size_t maxLen = 0;

    switch (attrID) {
      case 0x00: dest = appId;          maxLen = sizeof(appId);          break;
      case 0x01: dest = title;          maxLen = sizeof(title);          break;
      case 0x02: dest = subtitle;       maxLen = sizeof(subtitle);       break;
      case 0x03: dest = message;        maxLen = sizeof(message);        break;
      case 0x05: dest = dateTime;       maxLen = sizeof(dateTime);       break;
      case 0x06: dest = positiveAction; maxLen = sizeof(positiveAction); break;
      case 0x07: dest = negativeAction; maxLen = sizeof(negativeAction); break;
    }

    if (dest && maxLen > 0) {
      size_t copyLen = (attrLen < maxLen - 1) ? attrLen : maxLen - 1;
      
      if (copyLen > 512) {
        Serial.printf(">> ERROR: copyLen=%d too large, clamping\n", copyLen);
        copyLen = 512;
      }
      
      if (processingBuffer + pos == nullptr || (void*)(processingBuffer + pos) == (void*)0xFFFFFFFF) {
        Serial.println(">> ERROR: Invalid source pointer in memcpy");
        processingBufferSize = 0;
        return;
      }
      
      memcpy(dest, processingBuffer + pos, copyLen);
      dest[copyLen] = '\0';

      if (copyLen < maxLen) {
        for (size_t i = 0; i < copyLen; i++) {
          if (dest[i] == 0) dest[i] = ' ';
        }
        dest[copyLen] = '\0';
      }
    }

    pos += attrLen;
  }

  size_t appIdLen = safe_strlen(appId, sizeof(appId));
  size_t titleLen = safe_strlen(title, sizeof(title));
  size_t messageLen = safe_strlen(message, sizeof(message));
  
  Serial.printf("  [APP]: %s\n", appIdLen > 0 ? appId : "(empty)");
  Serial.printf("  [TITLE]: %s\n", titleLen > 0 ? title : "(empty)");
  Serial.printf("  [MSG]: %s\n", messageLen > 0 ? message : "(empty)");
  Serial.printf("  [POS ACTION]: %s\n", positiveAction);
  Serial.printf("  [NEG ACTION]: %s\n", negativeAction);

  bool important = (nextEventFlags & 0x02) != 0;

  if (nextEventCategory == 1) {
    Serial.println(">> Incoming call detected!");
    handleIncomingCall(title, subtitle, uid);
  } else {
    bool hasPositive = safe_strlen(positiveAction, sizeof(positiveAction)) > 0;
    bool hasNegative = safe_strlen(negativeAction, sizeof(negativeAction)) > 0;

    // When a call is accepted, iOS removes the ringing notification and adds a new one
    // from com.apple.mobilephone (caller name as title, "End Call" as negative action).
    // Detect it and: (1) track its UID for end-of-call detection, (2) queue a UID update
    // so the call screen's "End Call" button targets the correct live UID, (3) suppress
    // it from the notification store so it doesn't appear as a regular notification.
    if (notificationStore.isCallInProgress() &&
        activeCallNotificationUid == 0 &&
        safe_strlen(appId, sizeof(appId)) > 0 &&
        strstr(appId, "mobilephone") != nullptr) {
      activeCallNotificationUid = uid;
      notificationStore.activeCallUid = uid;
      notificationStore.queueSetActiveCallUid(uid);
      Serial.printf(">> Active Call UID %u tracked, INCOMING_CALL_UID update queued\n", uid);
    } else {
      notificationStore.queueAddNotification(
        appId, title, subtitle, message, dateTime,
        uid, nextEventCategory, important,
        hasPositive, hasNegative, positiveAction, negativeAction
      );
    }
  }

  processingBufferSize = 0;
  Serial.println(">> Notification processed");
}

void BLE::handleIncomingCall(const char* callerName, const char* callerNumber, uint32_t uid) {
  VALIDATE_PTR(callerName, "callerName");
  VALIDATE_PTR(callerNumber, "callerNumber");
  
  incomingCallUid = uid;
  
  size_t nameLen = safe_strlen(callerName, sizeof(lastCallName));
  size_t numberLen = safe_strlen(callerNumber, sizeof(lastCallNumber));
  
  if (nameLen > 0) {
    strncpy(lastCallName, callerName, sizeof(lastCallName) - 1);
    lastCallName[sizeof(lastCallName) - 1] = '\0';
  } else {
    strcpy(lastCallName, "Unknown Caller");
  }
  
  if (numberLen > 0) {
    strncpy(lastCallNumber, callerNumber, sizeof(lastCallNumber) - 1);
    lastCallNumber[sizeof(lastCallNumber) - 1] = '\0';
  } else {
    lastCallNumber[0] = '\0';
  }
  
  notificationStore.queuePushCallScreen(lastCallName, lastCallNumber, uid);
  Serial.printf(">> Queued IncomingCall screen: %s\n", lastCallName);
}

void BLE::resetRemotePointers() {
  // FIX: Increment connection generation FIRST to invalidate all outstanding local copies
  connectionGeneration++;
 
  if (blePointerMutex && xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    pClient = nullptr;
    pCPChar = nullptr;
    
    pAMSRemoteCommand = nullptr;
    pAMSEntityUpdate = nullptr;
    pAMSEntityAttribute = nullptr;
    
    xSemaphoreGive(blePointerMutex);
  }
  
  subscribed = false;
  waitingForResponse = false;
  nextEventActive = false;
  dataBufferSize = 0;
  processingBufferSize = 0;
  
  amsConnected = false;
  amsNeedsInitialRequest = false;
  amsNeedsTrackRefresh = false;
  amsDiscoveryAttempted = false;
  amsDataReady = false;
  amsUpdateBufferSize = 0;
  connectionEstablishedTime = 0;  // FIX: Reset connection timing
  bleOperationInProgress = false;  // FIX: Clear operation lock
  consecutiveReadFailures = 0;     // FIX: Reset failure counter
  
  // FIX: Clear ALL buffers on disconnect so UI always sees empty state
  if (writeBufferPtr && displayBuffersPtr) {
    memset(&writeBufferPtr->data, 0, sizeof(AMSMediaState));
    memset(&displayBuffersPtr[0].data, 0, sizeof(AMSMediaState));
    memset(&displayBuffersPtr[1].data, 0, sizeof(AMSMediaState));
  }
  
  elapsedTimeAnchor = 0.0f;
  elapsedTimeTimestamp = 0;
  
  // FIX: Signal Core 1 to clear media UI variables instead of calling setGlobalVariable from Core 0
  // The main loop will detect amsConnected==false and clear the UI
  mediaUIUpdateNeeded = true;
}

void BLE::MyNimBLEServerCallbacks::onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
  if (!bleParent || !bleParent->blePointerMutex) return;

  if (xSemaphoreTake(bleParent->blePointerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
    bleParent->pClient = pServer->getClient(connInfo);
    if (bleParent->pClient) {
      bleParent->pClient->exchangeMTU();
    }
    xSemaphoreGive(bleParent->blePointerMutex);
    
    if (bleParent->pClient) {
      Serial.println(">> Connected. Waiting for encryption...");
    }
  } else {
    Serial.println(">> WARNING: Could not acquire mutex in onConnect");
  }
}

void BLE::MyNimBLEServerCallbacks::onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
  if (!bleParent) return;

  Serial.printf(">> Disconnected (reason: %d)\n", reason);
  bleParent->resetRemotePointers();
  NimBLEDevice::getAdvertising()->start();
}

void BLE::sendAction(uint32_t uid, bool isPositive) {
  if (!blePointerMutex) {
    Serial.println(">> Error: BLE mutex not initialized");
    return;
  }

  // FIX: Capture connection gen and connHandle under mutex, then release before I/O.
  // The previous implementation held the mutex across writeValue() which blocks the
  // BLE callback task (deadlock risk) and also could not safely call ble_gap_conn_find().
  uint32_t savedGen = connectionGeneration.load();
  
  NimBLERemoteCharacteristic* localCP = nullptr;
  uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
  
  if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println(">> Error: Could not acquire BLE mutex");
    return;
  }

  bool canSend = (pCPChar != nullptr && pClient != nullptr && pClient->isConnected());
  if (canSend) {
    localCP = pCPChar;
    connHandle = pClient->getConnHandle();
  }
  xSemaphoreGive(blePointerMutex);

  if (!canSend || !localCP) {
    Serial.println(">> Error: BLE not connected or CP char unavailable");
    return;
  }
  
  // FIX: Validate the BLE host descriptor before attempting I/O.
  // See safeWriteValue for full explanation of the EXCVADDR 0x00000043 crash.
  if (!isConnectionValid(savedGen) || !isDescriptorValid(connHandle)) {
    Serial.println(F(">> sendAction: BLE descriptor freed – aborting action"));
    return;
  }

  uint8_t packet[6] = {
    0x02,  // PerformNotificationAction
    (uint8_t)(uid & 0xFF),
    (uint8_t)((uid >> 8) & 0xFF),
    (uint8_t)((uid >> 16) & 0xFF),
    (uint8_t)((uid >> 24) & 0xFF),
    (uint8_t)(isPositive ? 0x00 : 0x01)
  };

  bool success = safeWriteValue(localCP, packet, sizeof(packet), true, savedGen);
  
  if (success) {
    Serial.printf(">> Action sent successfully for UID %u\n", uid);
  } else {
    Serial.printf(">> ERROR: Failed to send action for UID %u\n", uid);
  }
}

// ============================================================================
// FIX: Safe BLE I/O wrappers
// These check connection validity before AND after the operation,
// and prevent concurrent operations that can confuse NimBLE's response handlers.
// Returns false if connection dropped or operation failed.
// ============================================================================
bool BLE::safeWriteValue(NimBLERemoteCharacteristic* pChar, uint8_t* data, size_t len, 
                         bool withResponse, uint32_t savedGen) {
  if (!pChar || !isConnectionValid(savedGen)) return false;
  
  // Prevent concurrent BLE operations
  bool expected = false;
  if (!bleOperationInProgress.compare_exchange_strong(expected, true)) {
    Serial.println(F(">> BLE operation already in progress, skipping write"));
    return false;
  }
  
  bool success = false;
  try {
    // FIX: Capture the connection handle INSIDE the mutex so it's coherent with
    // the connection pointer check. We then use it OUTSIDE the mutex to call
    // ble_gap_conn_find(), which is safe to call without holding blePointerMutex.
    uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
    if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      bool connected = (pClient && pClient->isConnected());
      
      // NOTE: Do NOT call pClient->getRssi() here. When the BLE link is stale
      // (phone asleep/out of range), NimBLE reports isConnected()==true but its
      // internal connection descriptor is already freed. getRssi() dereferences
      // that freed descriptor → LoadProhibited at EXCVADDR 0x00000043.
      // This is a hardware fault, not catchable by try/catch.
      
      if (connected) {
        connHandle = pClient->getConnHandle();
      }
      xSemaphoreGive(blePointerMutex);
      if (!connected || !isConnectionValid(savedGen)) {
        bleOperationInProgress = false;
        return false;
      }
    } else {
      bleOperationInProgress = false;
      return false;
    }
    
    // FIX: This is the critical guard that prevents EXCVADDR 0x00000043 crashes.
    //
    // The sequence that causes the crash:
    //   1. Phone goes to sleep / BLE hardware link goes stale
    //   2. NimBLE has not yet processed the disconnect event
    //   3. isConnected() returns true (it checks a cached flag, not the descriptor)
    //   4. writeValue() calls into NimBLE host layer, which dereferences
    //      struct ble_hs_conn at offset 0x43 — already freed by the BLE stack
    //   5. LoadProhibited hardware fault fires — NOT catchable by try/catch on Xtensa
    //   6. Backtrace appears CORRUPTED because the exception handler itself
    //      fails to walk the NimBLE host task's unframed assembly stack
    //
    // ble_gap_conn_find() queries the live host connection table directly.
    // If it returns non-zero (BLE_HS_ENOTCONN), the descriptor is gone and we
    // bail out here, safely, before any I/O attempt touches freed memory.
    if (!isDescriptorValid(connHandle)) {
      Serial.println(F(">> safeWriteValue: BLE descriptor freed (phone asleep?) – aborting write"));
      bleOperationInProgress = false;
      return false;
    }
    
    success = pChar->writeValue(data, len, withResponse);
  } catch (...) {
    Serial.println(F(">> Exception in safeWriteValue"));
    success = false;
  }
  
  bleOperationInProgress = false;
  return success && isConnectionValid(savedGen);
}

bool BLE::safeReadValue(NimBLERemoteCharacteristic* pChar, NimBLEAttValue& outValue, 
                        uint32_t savedGen) {
  if (!pChar || !isConnectionValid(savedGen)) return false;
  
  // Prevent concurrent BLE operations
  bool expected = false;
  if (!bleOperationInProgress.compare_exchange_strong(expected, true)) {
    Serial.println(F(">> BLE operation already in progress, skipping read"));
    return false;
  }
  
  bool success = false;
  try {
    // FIX: Capture the connection handle INSIDE the mutex (mirrors safeWriteValue).
    uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
    if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      bool connected = (pClient && pClient->isConnected());
      
      // NOTE: Do NOT call pClient->getRssi() here — see safeWriteValue comment.
      
      if (connected) {
        connHandle = pClient->getConnHandle();
      }
      xSemaphoreGive(blePointerMutex);
      if (!connected || !isConnectionValid(savedGen)) {
        bleOperationInProgress = false;
        return false;
      }
    } else {
      bleOperationInProgress = false;
      return false;
    }
    
    // FIX: Same ble_gap_conn_find() guard as safeWriteValue.
    // readValue() is equally capable of triggering the LoadProhibited crash at
    // EXCVADDR 0x00000043 when the BLE host descriptor has been freed.
    if (!isDescriptorValid(connHandle)) {
      Serial.println(F(">> safeReadValue: BLE descriptor freed (phone asleep?) – aborting read"));
      bleOperationInProgress = false;
      return false;
    }
    
    outValue = pChar->readValue();
    success = (outValue.length() > 0);
  } catch (...) {
    Serial.println(F(">> Exception in safeReadValue"));
    success = false;
  }
  
  bleOperationInProgress = false;
  return success && isConnectionValid(savedGen);
}

void BLE::sendMediaCommand(AMSRemoteCommand cmd) {
  static unsigned long lastAMSConnectTime = 0;
  static bool wasConnected = false;
  
  bool currentlyConnected = amsConnected.load();
  if (currentlyConnected && !wasConnected) {
    lastAMSConnectTime = millis();
    Serial.println(F(">> AMS connection state changed to connected"));
  }
  wasConnected = currentlyConnected;
  
  if (!currentlyConnected) {
    Serial.println(F(">> AMS not connected"));
    return;
  }
  
  if (!blePointerMutex) {
    Serial.println(F(">> Error: BLE mutex not initialized"));
    return;
  }
  
  // FIX: Capture connection generation BEFORE taking mutex
  uint32_t savedGen = connectionGeneration.load();
  
  if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println(F(">> Error: Could not acquire BLE mutex for media command"));
    return;
  }
  
  if (!amsConnected.load()) {
    Serial.println(F(">> AMS disconnected during mutex wait"));
    xSemaphoreGive(blePointerMutex);
    return;
  }
  
  NimBLERemoteCharacteristic* localCmd = pAMSRemoteCommand;
  NimBLEClient* localClient = pClient;
  
  if (!localCmd || !localClient || !localClient->isConnected()) {
    Serial.println(F(">> Error: Cannot send - BLE not ready"));
    xSemaphoreGive(blePointerMutex);
    return;
  }
  
  xSemaphoreGive(blePointerMutex);
  
  // FIX: Verify connection generation hasn't changed (no disconnect occurred)
  if (!isConnectionValid(savedGen)) {
    Serial.println(F(">> Connection changed during media command preparation"));
    return;
  }
  
  // FIX: Re-verify connection immediately before write
  if (!localClient->isConnected()) {
    Serial.println(F(">> Connection lost before media command write"));
    return;
  }
  
  uint8_t cmdByte = (uint8_t)cmd;
  bool success = false;
  
  // FIX: Use safeWriteValue which prevents concurrent operations and checks connection
  success = safeWriteValue(localCmd, &cmdByte, 1, true, savedGen);
  
  if (!success && isConnectionValid(savedGen) && localClient->isConnected()) {
    // Retry without response - no pending handler to corrupt
    Serial.println(F(">> Write with response failed, trying without..."));
    success = safeWriteValue(localCmd, &cmdByte, 1, false, savedGen);
  }
  
  if (success) {
    Serial.println(F(">> Media command sent successfully"));
    vTaskDelay(pdMS_TO_TICKS(50));
  } else {
    Serial.println(F(">> ERROR: Failed to send media command"));
  }
}

void BLE::requestTrackInfo() {
  if (!amsConnected.load() || !blePointerMutex) return;
  
  // FIX: Don't do BLE I/O during connection stabilization period
  if (!isConnectionStable()) return;

  // FIX: Capture connection generation at the start
  uint32_t savedGen = connectionGeneration.load();
  
  // FIX: Write to Core 0's private write buffer (accumulates all changes)
  AMSMediaState* writeBuffer = getWriteBuffer();

  uint8_t attributes[] = {
    AMS_TRACK_ATTRIBUTE_ARTIST,
    AMS_TRACK_ATTRIBUTE_ALBUM,
    AMS_TRACK_ATTRIBUTE_TITLE,
    AMS_TRACK_ATTRIBUTE_DURATION
  };

  int failuresThisCall = 0;

  for (int i = 0; i < 4; i++) {
    if (!amsConnected.load()) return;
    if (!isConnectionValid(savedGen)) {
      Serial.println(F(">> Connection changed during requestTrackInfo, aborting"));
      return;
    }
    
    NimBLERemoteCharacteristic* localAttr = nullptr;
    NimBLEClient* localClient = nullptr;
    
    if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      localAttr = pAMSEntityAttribute;
      localClient = pClient;
      xSemaphoreGive(blePointerMutex);
    }
    
    if (!localAttr || !localClient || !localClient->isConnected()) return;
    if (!isConnectionValid(savedGen)) return;
    
    try {
      // Entity Attribute protocol requires write-WITH-response for iOS to register the selection
      uint8_t request[] = {AMS_ENTITY_TRACK, attributes[i]};
      if (!safeWriteValue(localAttr, request, sizeof(request), true, savedGen)) {
        Serial.printf(">> EA write failed for track attr %d\n", attributes[i]);
        failuresThisCall++;
        continue;
      }
      
      
      // FIX: Use safeReadValue which checks connection before and after
      NimBLEAttValue response;
      if (!safeReadValue(localAttr, response, savedGen)) {
        Serial.printf(">> EA read failed for track attr %d\n", attributes[i]);
        failuresThisCall++;
        continue;
      }
      
      // Successful read - reset consecutive failure counter
      consecutiveReadFailures = 0;
      
      if (response.length() >= 1) {
        const char* value = (const char*)response.data();
        size_t valueLen = response.length();
        
        char valueStr[256];
        size_t copyLen = (valueLen < sizeof(valueStr) - 1) ? valueLen : (sizeof(valueStr) - 1);
        memcpy(valueStr, value, copyLen);
        valueStr[copyLen] = '\0';
                      
        switch (attributes[i]) {
          case AMS_TRACK_ATTRIBUTE_ARTIST:
            strncpy(writeBuffer->trackArtist, valueStr, sizeof(writeBuffer->trackArtist) - 1);
            writeBuffer->trackArtist[sizeof(writeBuffer->trackArtist) - 1] = '\0';
            break;
          case AMS_TRACK_ATTRIBUTE_ALBUM:
            strncpy(writeBuffer->trackAlbum, valueStr, sizeof(writeBuffer->trackAlbum) - 1);
            writeBuffer->trackAlbum[sizeof(writeBuffer->trackAlbum) - 1] = '\0';
            break;
          case AMS_TRACK_ATTRIBUTE_TITLE:
            {
              const char* bullet = strstr(valueStr, " • ");
              if (bullet) {
                size_t titleLen = bullet - valueStr;
                if (titleLen < sizeof(writeBuffer->trackTitle)) {
                  memcpy(writeBuffer->trackTitle, valueStr, titleLen);
                  writeBuffer->trackTitle[titleLen] = '\0';
                } else {
                  strncpy(writeBuffer->trackTitle, valueStr, sizeof(writeBuffer->trackTitle) - 1);
                  writeBuffer->trackTitle[sizeof(writeBuffer->trackTitle) - 1] = '\0';
                }
                
                const char* artistStart = bullet + 3;
                strncpy(writeBuffer->trackArtist, artistStart, sizeof(writeBuffer->trackArtist) - 1);
                writeBuffer->trackArtist[sizeof(writeBuffer->trackArtist) - 1] = '\0';
              } else {
                strncpy(writeBuffer->trackTitle, valueStr, sizeof(writeBuffer->trackTitle) - 1);
                writeBuffer->trackTitle[sizeof(writeBuffer->trackTitle) - 1] = '\0';
              }
              writeBuffer->validTrackInfo = true;
              // FIX: No longer need mediaStateValid - buffer swap makes it atomic
            }
            break;
          case AMS_TRACK_ATTRIBUTE_DURATION:
            strncpy(writeBuffer->trackDuration, valueStr, sizeof(writeBuffer->trackDuration) - 1);
            writeBuffer->trackDuration[sizeof(writeBuffer->trackDuration) - 1] = '\0';
            break;
        }
      }
    } catch (...) {
      Serial.printf(">> Exception reading track attr %d\n", attributes[i]);
      failuresThisCall++;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  
  // FIX: Track consecutive failures across calls. If reads keep failing,
  // the BLE link is degraded (phone asleep, out of range) and NimBLE's internal
  // characteristic objects may become invalid — continued access will crash.
  consecutiveReadFailures += failuresThisCall;
  if (consecutiveReadFailures >= MAX_CONSECUTIVE_READ_FAILURES) {
    Serial.printf(">> %d consecutive read failures - AMS link degraded, disconnecting\n",
                  consecutiveReadFailures);
    amsConnected = false;
    // FIX: No need to set mediaStateValid - buffers will be cleared on disconnect
    consecutiveReadFailures = 0;
  }
  
  // FIX: Copy writeBuffer → inactive display buffer, then atomically swap
  // All track info updates become visible to Core 1 at once
  swapBuffers();
  
  // FIX: Signal Core 1 to update UI instead of calling setGlobalVariable from Core 0
  mediaUIUpdateNeeded = true;
}

void BLE::requestPlayerInfo() {
  if (!amsConnected.load() || !blePointerMutex) return;
  
  // FIX: Don't do BLE I/O during connection stabilization period
  if (!isConnectionStable()) return;

  // FIX: Capture connection generation at the start
  uint32_t savedGen = connectionGeneration.load();
  
  // FIX: Write to Core 0's private write buffer (accumulates all changes)
  AMSMediaState* writeBuffer = getWriteBuffer();

  uint8_t attributes[] = {
    AMS_PLAYER_ATTRIBUTE_NAME,
    AMS_PLAYER_ATTRIBUTE_PLAYBACK_INFO
  };

  int failuresThisCall = 0;

  for (int i = 0; i < 2; i++) {
    if (!amsConnected.load()) return;
    if (!isConnectionValid(savedGen)) return;
    
    NimBLERemoteCharacteristic* localAttr = nullptr;
    NimBLEClient* localClient = nullptr;
    
    if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      localAttr = pAMSEntityAttribute;
      localClient = pClient;
      xSemaphoreGive(blePointerMutex);
    }
    
    if (!localAttr || !localClient || !localClient->isConnected()) return;
    if (!isConnectionValid(savedGen)) return;
    
    try {
      // Entity Attribute protocol requires write-WITH-response
      uint8_t request[] = {AMS_ENTITY_PLAYER, attributes[i]};
      if (!safeWriteValue(localAttr, request, sizeof(request), true, savedGen)) {
        Serial.printf(">> EA write failed for player attr %d\n", attributes[i]);
        failuresThisCall++;
        continue;
      }
      
      
      // FIX: Use safeReadValue
      NimBLEAttValue response;
      if (!safeReadValue(localAttr, response, savedGen)) {
        Serial.printf(">> EA read failed for player attr %d\n", attributes[i]);
        failuresThisCall++;
        continue;
      }
      
      // Successful read - reset consecutive failure counter
      consecutiveReadFailures = 0;
      
      if (response.length() >= 1) {
        const char* value = (const char*)response.data();
        size_t valueLen = response.length();
        
        char valueStr[256];
        size_t copyLen = (valueLen < sizeof(valueStr) - 1) ? valueLen : (sizeof(valueStr) - 1);
        memcpy(valueStr, value, copyLen);
        valueStr[copyLen] = '\0';              
        
        switch (attributes[i]) {
          case AMS_PLAYER_ATTRIBUTE_NAME:
            strncpy(writeBuffer->playerName, valueStr, sizeof(writeBuffer->playerName) - 1);
            writeBuffer->playerName[sizeof(writeBuffer->playerName) - 1] = '\0';
            break;
            
          case AMS_PLAYER_ATTRIBUTE_PLAYBACK_INFO: {
            const char* comma1 = strchr(valueStr, ',');
            if (comma1) {
              writeBuffer->playbackState = atoi(valueStr);
              const char* comma2 = strchr(comma1 + 1, ',');
              if (comma2) {
                float et = atof(comma2 + 1);
                writeBuffer->elapsedTime = et;
                elapsedTimeAnchor = et;
                elapsedTimeTimestamp = millis();
              }
              writeBuffer->validPlayerInfo = true;
            }
            break;
          }
        }
      }
    } catch (...) {
      Serial.printf(">> Exception reading player attr %d\n", attributes[i]);
      failuresThisCall++;
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  
  // FIX: Accumulate failures across calls
  consecutiveReadFailures += failuresThisCall;
  if (consecutiveReadFailures >= MAX_CONSECUTIVE_READ_FAILURES) {
    Serial.printf(">> %d consecutive read failures - AMS link degraded, disconnecting\n",
                  consecutiveReadFailures);
    amsConnected = false;
    // FIX: No need to set mediaStateValid - buffers will be cleared on disconnect
    consecutiveReadFailures = 0;
  }
  
  // FIX: Copy writeBuffer → inactive display buffer, then atomically swap
  // All player info updates become visible to Core 1 at once
  swapBuffers();
  
  mediaUIUpdateNeeded = true;
}

void BLE::requestQueueInfo() {
  if (!amsConnected.load() || !blePointerMutex) return;
  if (!isConnectionStable()) return;

  uint32_t savedGen = connectionGeneration.load();
  
  // FIX: Write to Core 0's private write buffer (accumulates all changes)
  AMSMediaState* writeBuffer = getWriteBuffer();

  uint8_t attributes[] = {
    AMS_QUEUE_ATTRIBUTE_INDEX,
    AMS_QUEUE_ATTRIBUTE_COUNT,
    AMS_QUEUE_ATTRIBUTE_SHUFFLE_MODE,
    AMS_QUEUE_ATTRIBUTE_REPEAT_MODE
  };

  for (int i = 0; i < 4; i++) {
    if (!amsConnected.load()) return;
    if (!isConnectionValid(savedGen)) return;
    
    NimBLERemoteCharacteristic* localAttr = nullptr;
    NimBLEClient* localClient = nullptr;
    
    if (xSemaphoreTake(blePointerMutex, pdMS_TO_TICKS(100)) == pdTRUE) {
      localAttr = pAMSEntityAttribute;
      localClient = pClient;
      xSemaphoreGive(blePointerMutex);
    }
    
    if (!localAttr || !localClient || !localClient->isConnected()) return;
    if (!isConnectionValid(savedGen)) return;
    
    try {
      uint8_t request[] = {AMS_ENTITY_QUEUE, attributes[i]};
      if (!safeWriteValue(localAttr, request, sizeof(request), true, savedGen)) continue;
      
      NimBLEAttValue response;
      if (!safeReadValue(localAttr, response, savedGen)) continue;
      
      if (response.length() >= 3) {
        const char* value = (const char*)response.data() + 2;
        size_t valueLen = response.length() - 2;
        
        char valueStr[64];
        size_t copyLen = (valueLen < sizeof(valueStr) - 1) ? valueLen : (sizeof(valueStr) - 1);
        memcpy(valueStr, value, copyLen);
        valueStr[copyLen] = '\0';
        
        switch (attributes[i]) {
          case AMS_QUEUE_ATTRIBUTE_INDEX:
            writeBuffer->queueIndex = atoi(valueStr);
            break;
          case AMS_QUEUE_ATTRIBUTE_COUNT:
            writeBuffer->queueCount = atoi(valueStr);
            writeBuffer->validQueueInfo = true;
            break;
          case AMS_QUEUE_ATTRIBUTE_SHUFFLE_MODE:
            writeBuffer->shuffleMode = atoi(valueStr);
            break;
          case AMS_QUEUE_ATTRIBUTE_REPEAT_MODE:
            writeBuffer->repeatMode = atoi(valueStr);
            break;
        }
      }
    } catch (...) {
      Serial.printf(">> Exception reading queue attr %d\n", attributes[i]);
    }
    
    vTaskDelay(pdMS_TO_TICKS(50));
  }
  
  // FIX: Copy writeBuffer → inactive display buffer, then atomically swap
  // All queue info updates become visible to Core 1 at once
  swapBuffers();
}

// Called from BLE callback (Core 0) - just copy to buffer
void BLE::amsEntityUpdateCallback(uint8_t* pData, size_t length) {
  VALIDATE_PTR(pData, "pData");
  VALIDATE_PTR(amsUpdateBuffer, "amsUpdateBuffer");
  
  if (length > amsUpdateBufferCapacity) {
    Serial.printf(">> AMS update too large: %d bytes\n", length);
    return;
  }
  
  memcpy(amsUpdateBuffer, pData, length);
  amsUpdateBufferSize = length;
  // FIX: release store — guarantees the memcpy above is fully visible to Core 1
  // before Core 1's acquire-load of amsDataReady returns true.
  // Without this, Core 1 can observe amsDataReady=true but read stale/partial
  // amsUpdateBuffer data, producing garbled AMS attribute values and potentially
  // corrupt LVGL state downstream. Mirrors the release/acquire pair already
  // applied to artwork_ready in media_controls.cpp.
  amsDataReady.store(true, std::memory_order_release);
}

// Called from main loop (Core 1) - processes buffered data
void BLE::processAMSEntityUpdate() {
  // FIX: acquire load — pairs with release store in amsEntityUpdateCallback (Core 0).
  // Guarantees all writes to amsUpdateBuffer before the store are visible here.
  if (!amsDataReady.load(std::memory_order_acquire)) {
    return;
  }
  
  VALIDATE_PTR(amsUpdateBuffer, "amsUpdateBuffer");
  
  parseAMSEntityUpdate(amsUpdateBuffer, amsUpdateBufferSize);
  // FIX: relaxed store is sufficient — Core 1 only clears this, Core 0 sets it.
  // We only need the acquire on the load above (producer-consumer ordering).
  amsDataReady.store(false, std::memory_order_relaxed);
}

void BLE::parseAMSEntityUpdate(uint8_t* pData, size_t length) {
  VALIDATE_PTR(pData, "pData in parseAMSEntityUpdate");
  
  if (length < 3) return;
  
  // FIX: Write to Core 0's private write buffer (never read by Core 1)
  // Changes accumulate here and are copied to display buffer on swap
  AMSMediaState* writeBuffer = getWriteBuffer();
  
  uint8_t entityID = pData[0];
  uint8_t attributeID = pData[1];
  uint8_t flags = pData[2];
  
  const char* value = (const char*)&pData[3];
  size_t valueLen = length - 3;
  
  char valueStr[256];
  size_t copyLen = (valueLen < sizeof(valueStr) - 1) ? valueLen : (sizeof(valueStr) - 1);
  memcpy(valueStr, value, copyLen);
  valueStr[copyLen] = '\0';  
  
  bool trackChanged = false;
  static char previousAlbum[128] = {0};
  static uint8_t previousPlaybackState = 0xFF;
  
  switch (entityID) {
    case AMS_ENTITY_TRACK:
      switch (attributeID) {
        case AMS_TRACK_ATTRIBUTE_ARTIST:
          Serial.printf(">> ARTIST UPDATE: '%s'\n", valueStr);
          strncpy(writeBuffer->trackArtist, valueStr, sizeof(writeBuffer->trackArtist) - 1);
          writeBuffer->trackArtist[sizeof(writeBuffer->trackArtist) - 1] = '\0';
          break;
          
        case AMS_TRACK_ATTRIBUTE_ALBUM:
          Serial.printf(">> ALBUM UPDATE: '%s'\n", valueStr);
          if (safe_strlen(previousAlbum, sizeof(previousAlbum)) > 0 && 
              strcmp(previousAlbum, valueStr) != 0) {
            trackChanged = true;
          }
          strncpy(writeBuffer->trackAlbum, valueStr, sizeof(writeBuffer->trackAlbum) - 1);
          writeBuffer->trackAlbum[sizeof(writeBuffer->trackAlbum) - 1] = '\0';
          strncpy(previousAlbum, valueStr, sizeof(previousAlbum) - 1);
          previousAlbum[sizeof(previousAlbum) - 1] = '\0';
          break;
          
        case AMS_TRACK_ATTRIBUTE_TITLE: {
          Serial.printf(">> TITLE UPDATE: '%s'\n", valueStr);
          
          const char* bullet = strstr(valueStr, " • ");
          if (bullet) {
            size_t titleLen = bullet - valueStr;
            char extractedTitle[128];
            if (titleLen < sizeof(extractedTitle)) {
              memcpy(extractedTitle, valueStr, titleLen);
              extractedTitle[titleLen] = '\0';
            } else {
              strncpy(extractedTitle, valueStr, sizeof(extractedTitle) - 1);
              extractedTitle[sizeof(extractedTitle) - 1] = '\0';
            }
            
            const char* artistStart = bullet + 3;
            strncpy(writeBuffer->trackArtist, artistStart, sizeof(writeBuffer->trackArtist) - 1);
            writeBuffer->trackArtist[sizeof(writeBuffer->trackArtist) - 1] = '\0';
            
            if (safe_strlen(previousTrackTitle, sizeof(previousTrackTitle)) > 0 && 
                strcmp(previousTrackTitle, extractedTitle) != 0) {
              trackChanged = true;
            }
            
            strncpy(writeBuffer->trackTitle, extractedTitle, sizeof(writeBuffer->trackTitle) - 1);
            writeBuffer->trackTitle[sizeof(writeBuffer->trackTitle) - 1] = '\0';
            strncpy(previousTrackTitle, extractedTitle, sizeof(previousTrackTitle) - 1);
            previousTrackTitle[sizeof(previousTrackTitle) - 1] = '\0';
          } else {
            if (safe_strlen(previousTrackTitle, sizeof(previousTrackTitle)) > 0 && 
                strcmp(previousTrackTitle, valueStr) != 0) {
              trackChanged = true;
            }
            strncpy(writeBuffer->trackTitle, valueStr, sizeof(writeBuffer->trackTitle) - 1);
            writeBuffer->trackTitle[sizeof(writeBuffer->trackTitle) - 1] = '\0';
            strncpy(previousTrackTitle, valueStr, sizeof(previousTrackTitle) - 1);
            previousTrackTitle[sizeof(previousTrackTitle) - 1] = '\0';
          }
          
          writeBuffer->validTrackInfo = true;
          // FIX: No longer need mediaStateValid flag - buffer swap makes it atomic
          break;
        }
          
        case AMS_TRACK_ATTRIBUTE_DURATION:
          strncpy(writeBuffer->trackDuration, valueStr, sizeof(writeBuffer->trackDuration) - 1);
          writeBuffer->trackDuration[sizeof(writeBuffer->trackDuration) - 1] = '\0';
          break;
          
        default:
          Serial.printf(">> Unknown track attribute: %d\n", attributeID);
          break;
      }
      break;
      
    case AMS_ENTITY_PLAYER:
      switch (attributeID) {
        case AMS_PLAYER_ATTRIBUTE_NAME:
          Serial.printf(">> PLAYER NAME: '%s'\n", valueStr);
          strncpy(writeBuffer->playerName, valueStr, sizeof(writeBuffer->playerName) - 1);
          writeBuffer->playerName[sizeof(writeBuffer->playerName) - 1] = '\0';
          break;
          
        case AMS_PLAYER_ATTRIBUTE_PLAYBACK_INFO: {
          const char* comma1 = strchr(valueStr, ',');
          if (comma1) {
            uint8_t newState = atoi(valueStr);
            
            const char* comma2 = strchr(comma1 + 1, ',');
            float elapsedTime = 0.0f;
            if (comma2) {
              elapsedTime = atof(comma2 + 1);
              writeBuffer->elapsedTime = elapsedTime;
              elapsedTimeAnchor = elapsedTime;
              elapsedTimeTimestamp = millis();
              
              if (elapsedTime < 1.0f && previousPlaybackState == AMS_PLAYBACK_STATE_PLAYING) {
                Serial.println(F(">> *** POSSIBLE TRACK CHANGE (PLAYBACK RESTART) ***"));
                trackChanged = true;
              }
            }
            
            if (newState != previousPlaybackState) {
              Serial.printf(">> Playback state changed: %d -> %d\n", 
                           previousPlaybackState, newState);
              previousPlaybackState = newState;
            }
            
            writeBuffer->playbackState = newState;
            writeBuffer->validPlayerInfo = true;
          }
          break;
        }
        
        case AMS_PLAYER_ATTRIBUTE_VOLUME:
          strncpy(writeBuffer->volume, valueStr, sizeof(writeBuffer->volume) - 1);
          writeBuffer->volume[sizeof(writeBuffer->volume) - 1] = '\0';
          break;
          
        default:
          Serial.printf(">> Unknown player attribute: %d\n", attributeID);
          break;
      }
      break;
      
    case AMS_ENTITY_QUEUE:
      switch (attributeID) {
        case AMS_QUEUE_ATTRIBUTE_INDEX:
          writeBuffer->queueIndex = atoi(valueStr);
          break;
        case AMS_QUEUE_ATTRIBUTE_COUNT:
          writeBuffer->queueCount = atoi(valueStr);
          writeBuffer->validQueueInfo = true;
          break;
        case AMS_QUEUE_ATTRIBUTE_SHUFFLE_MODE:
          writeBuffer->shuffleMode = atoi(valueStr);
          break;
        case AMS_QUEUE_ATTRIBUTE_REPEAT_MODE:
          writeBuffer->repeatMode = atoi(valueStr);
          break;
      }
      break;
      
    default:
      Serial.printf(">> Unknown entity: %d\n", entityID);
      break;
  }
  
  if (trackChanged) {    
    amsNeedsTrackRefresh = true;
  }
  
  // FIX: ATOMIC BUFFER SWAP - copy writeBuffer → inactive display buffer, then swap
  // Core 1 was reading from the old active buffer the whole time (no torn reads possible)
  // After swap, Core 1 atomically switches to reading the newly updated display buffer
  swapBuffers();
  
  // FIX: Signal the UI loop to call updateMediaUIVariables() on the next tick instead
  // of calling it directly here. Direct call caused 2-3 redundant full LVGL update passes
  // per UI loop iteration: once here, again via the mediaUIUpdateNeeded check in the UI
  // loop, and again via the 1-second timer. Each pass calls eez::flow::setGlobalVariable()
  // for every media variable, triggering EEZ reactive LVGL redraws — so a single AMS
  // packet was causing 2-3x the expected LVGL redraw work per frame.
  // The mediaUIUpdateNeeded flag ensures exactly one updateMediaUIVariables() call per
  // AMS entity update, driven by the UI loop at a safe point in the LVGL frame cycle.
  mediaUIUpdateNeeded.store(true, std::memory_order_release);
}

void BLE::updateMediaUIVariables() {
  VALIDATE_PTR(this, "BLE object in updateMediaUIVariables");
  
  // FIX: Clear media UI when disconnected (safe - only called from Core 1 now)
  // Track connection transitions to clear UI exactly once on disconnect
  static bool wasConnected = false;
  
  if (!amsConnected.load()) {
    if (wasConnected) {
      wasConnected = false;
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_TITLE, eez::StringValue(""));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_ARTIST, eez::StringValue(""));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_ALBUM, eez::StringValue(""));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_DURATION, eez::StringValue("0:00"));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_TIME_REMAINING, eez::StringValue("0:00"));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_ELAPSED_TIME, eez::StringValue("0:00"));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_PROGRESS, eez::IntegerValue(0));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_IS_PLAYING, eez::BooleanValue(false));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_APP, eez::StringValue(""));
    }
    return;
  }
  
  // FIX: Mark as connected so next disconnect will clear UI
  wasConnected = true;
  
  // FIX: Use double-buffered read - always safe, no torn reads possible!
  // Core 0 writes to inactive buffer and swaps atomically
  const AMSMediaState* mediaState = getMediaState();
  
  static float previousDuration = 0.0f;
  static char previousTitle[128] = "";
  static char previousArtist[128] = "";
  
  static char formattedDuration[16];
  static char formattedRemaining[16];
  static char formattedElapsed[16];
  float remainingSecondsForPreconnect = -1.0f;
  
  strcpy(formattedDuration, "0:00");
  strcpy(formattedRemaining, "0:00");
  strcpy(formattedElapsed, "0:00");
  int progressPercent = 0;
  
  bool titleOrArtistChanged = false;
  if (strcmp(previousTitle, mediaState->trackTitle) != 0 || 
      strcmp(previousArtist, mediaState->trackArtist) != 0) {
    titleOrArtistChanged = true;
    strncpy(previousTitle, mediaState->trackTitle, sizeof(previousTitle) - 1);
    previousTitle[sizeof(previousTitle) - 1] = '\0';
    strncpy(previousArtist, mediaState->trackArtist, sizeof(previousArtist) - 1);
    previousArtist[sizeof(previousArtist) - 1] = '\0';
  }
  
  if (mediaState->trackDuration[0] != '\0') {
    float durationSeconds = atof(mediaState->trackDuration);
    
    if (durationSeconds > 0) {
      if (durationSeconds != previousDuration && previousDuration > 0) {
        if (elapsedTimeAnchor > durationSeconds || elapsedTimeAnchor > durationSeconds * 0.95f) {
          elapsedTimeAnchor = 0.0f;
          elapsedTimeTimestamp = millis();
        }
      }
      previousDuration = durationSeconds;
      
      int totalSec = (int)durationSeconds;
      int mins = totalSec / 60;
      int secs = totalSec % 60;
      snprintf(formattedDuration, sizeof(formattedDuration), "%d:%02d", mins, secs);
      
      float interpolatedElapsed = elapsedTimeAnchor;
      if (mediaState->playbackState == AMS_PLAYBACK_STATE_PLAYING && elapsedTimeTimestamp > 0) {
        float delta = (millis() - elapsedTimeTimestamp) / 1000.0f;
        interpolatedElapsed = elapsedTimeAnchor + delta;
      }
      
      if (interpolatedElapsed < 0) interpolatedElapsed = 0;
      if (interpolatedElapsed > durationSeconds) interpolatedElapsed = durationSeconds;
      
      int elapsedSec = (int)interpolatedElapsed;
      int elapsedMins = elapsedSec / 60;
      int elapsedSecs = elapsedSec % 60;
      snprintf(formattedElapsed, sizeof(formattedElapsed), "%d:%02d", elapsedMins, elapsedSecs);
      
      progressPercent = (int)((interpolatedElapsed / durationSeconds) * 100.0f);
      if (progressPercent > 100) progressPercent = 100;
      if (progressPercent < 0) progressPercent = 0;
      
      float remainingSeconds = durationSeconds - interpolatedElapsed;
      if (remainingSeconds < 0) remainingSeconds = 0;
      remainingSecondsForPreconnect = remainingSeconds;
      
      int remSec = (int)remainingSeconds;
      int remMins = remSec / 60;
      int remSecs = remSec % 60;
      snprintf(formattedRemaining, sizeof(formattedRemaining), "-%d:%02d", remMins, remSecs);
    }
  }
  
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_TITLE, 
                               eez::StringValue(mediaState->trackTitle));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_ARTIST, 
                               eez::StringValue(mediaState->trackArtist));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_ALBUM, 
                               eez::StringValue(mediaState->trackAlbum));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_DURATION, 
                               eez::StringValue(formattedDuration));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_TIME_REMAINING, 
                               eez::StringValue(formattedRemaining));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_ELAPSED_TIME, 
                               eez::StringValue(formattedElapsed));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_PROGRESS, 
                               eez::IntegerValue(progressPercent));
  
  bool isPlaying = (mediaState->playbackState == AMS_PLAYBACK_STATE_PLAYING);
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_IS_PLAYING, 
                               eez::BooleanValue(isPlaying));
  
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_MEDIA_APP, 
                               eez::StringValue(mediaState->playerName));

  mediaControls.update_playback_timing(mediaState->trackTitle,
                                       mediaState->trackArtist,
                                       isPlaying,
                                       remainingSecondsForPreconnect);

  bool forceArtwork = amsForceArtworkRefresh.exchange(false, std::memory_order_relaxed);
  if ((titleOrArtistChanged || forceArtwork) && mediaState->trackTitle[0] != '\0') {
    if (forceArtwork && !titleOrArtistChanged) {
      Serial.println(F(">> [AMS] Forcing artwork refresh for current track"));
    } else {
      Serial.println(F(">> Track changed - updating artwork"));
    }
    Serial.printf(">> Track: %s - %s\n", mediaState->trackArtist, mediaState->trackTitle);
    mediaControls.update_album_artwork(mediaState->trackTitle, mediaState->trackArtist, objects.media_image);
  }
}
