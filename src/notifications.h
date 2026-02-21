#pragma once

#ifndef NOTIFICATIONS_H
#define NOTIFICATIONS_H

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <atomic>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"  // CRITICAL FIX: Added for mutex support
#include "lvgl.h"

#define MAX_NOTIFICATIONS 50
#define QUICK_NOTIFICATION_DURATION_MS 7000  // 7 seconds

// Command types for thread-safe operations
enum class NotificationCommand : uint8_t {
  REMOVE,
  SHOW_NEXT,
  SHOW_PREV,
  SHOW_LATEST,
  POP_CALL_SCREEN,
  PUSH_CALL_SCREEN,          // Thread-safe incoming call screen push
  SHOW_QUICK_NOTIFICATION,
  ADD_NOTIFICATION,
  CLEAR_ACTIVE_CALL,         // Call truly ended — pop call screen + hide icon
  RETURN_TO_CALL,            // Re-push call screen from active-call icon tap
  SET_ACTIVE_CALL_UID        // Update INCOMING_CALL_UID EEZ var to active-call UID
};

struct NotificationCommandData {
  NotificationCommand cmd;
  uint32_t uid;  // Used for REMOVE command
};

struct StoredNotification {
  char iconId[32];
  char title[128];
  char subtitle[128];
  char message[256];
  char positiveActionLabel[32];
  char negativeActionLabel[32];
  time_t dateTime;
  uint32_t uid;
  uint8_t categoryId;
  bool important;
  bool hasPositiveAction;
  bool hasNegativeAction;
  bool viewed;
  bool valid;
};

// Data for quick notification display
struct QuickNotificationData {
  char icon[32];
  char title[128];
  char subtitle[128];
  char message[256];
  bool important;
};

// NEW: Data for queued notification add (thread-safe)
struct AddNotificationData {
  char appId[128];
  char title[128];
  char subtitle[128];
  char message[256];
  char dateTime[32];
  char positiveActionLabel[32];
  char negativeActionLabel[32];
  uint32_t uid;
  uint8_t categoryId;
  bool important;
  bool hasPositiveAction;
  bool hasNegativeAction;
};

// NEW: Data for incoming call screen (thread-safe)
struct IncomingCallData {
  char callerName[128];
  char callerNumber[64];
  uint32_t uid;
};

class NotificationStore {
public:
  bool begin();

  // Direct add - ONLY call from main loop/LVGL thread!
  bool addNotification(
    const char* appId,
    const char* title,
    const char* subtitle,
    const char* message,
    const char* dateTime,
    uint32_t uid,
    uint8_t categoryId,
    bool important,
    bool hasPositiveAction,
    bool hasNegativeAction,
    const char* positiveActionLabel,
    const char* negativeActionLabel
  );

  // NEW: Thread-safe queue for adding notifications from BLE callbacks
  void queueAddNotification(
    const char* appId,
    const char* title,
    const char* subtitle,
    const char* message,
    const char* dateTime,
    uint32_t uid,
    uint8_t categoryId,
    bool important,
    bool hasPositiveAction,
    bool hasNegativeAction,
    const char* positiveActionLabel,
    const char* negativeActionLabel
  );

  void queueRemove(uint32_t uid);
  void queuePopCallScreen();
  void queuePushCallScreen(const char* callerName, const char* callerNumber, uint32_t uid);
  void queueQuickNotification(const char* icon, const char* title, const char* subtitle, const char* message, bool important);

  // Active-call icon and screen control
  void queueClearActiveCall();              // BLE thread: call truly ended
  void queueReturnToCall();                 // UI thread: user taps active-call icon
  void queueSetActiveCallUid(uint32_t uid); // BLE thread: iOS sent replacement active-call UID
  bool isCallInProgress() const { return callInProgress.load(); }

  // Active-call state — written by action_accept_call (LVGL thread) BEFORE sendAction(),
  // read atomically by BLE Core 0 thread and by processCommands (LVGL thread).
  std::atomic<bool> callInProgress{false};
  char activeCallCallerName[128] = {0};
  char activeCallCallerNumber[64] = {0};
  uint32_t activeCallUid = 0;

  void processCommands();

  void showNext();
  void showPrevious();
  void showLatest();

  void updateDisplay();

  uint32_t getCurrentUID();
  bool currentHasPositiveAction();
  bool currentHasNegativeAction();

  int getTotalCount();
  int getUnviewedCount();
  int getCurrentIndex();

  void dismissQuickNotification();

private:
  void removeNotification(uint32_t uid);
  void setIconFromAppId(const char* appId, uint8_t categoryId, char* iconId, size_t maxLen);
  void sanitizeString(char* dest, const char* src, size_t maxLen);
  time_t parseDateTime(const char* dateTimeString);
  void formatRelativeTime(time_t timestamp, char* buffer, size_t bufferSize) const;
  int findOldestValidIndex();
  int findNewestValidIndex();
  void showQuickNotification(const QuickNotificationData& data);
  
  // Internal count functions (caller must hold mutex)
  int getTotalCountInternal();
  int getUnviewedCountInternal();

  StoredNotification* notifications = nullptr;

  std::atomic<int> totalCount{0};
  std::atomic<int> currentIndex{-1};
  std::atomic<int> newestIndex{-1};
  std::atomic<bool> initialized{false};

  // CRITICAL FIX: Mutex for protecting notifications array access across cores
  SemaphoreHandle_t notificationsMutex = nullptr;

  // Command queue for thread-safe operations
  QueueHandle_t commandQueue = nullptr;
  
  // Quick notification queue (separate to hold larger data)
  QueueHandle_t quickNotificationQueue = nullptr;
  
  // Add notification queue (for thread-safe adds from BLE)
  QueueHandle_t addNotificationQueue = nullptr;
  
  // Incoming call queue (for thread-safe call screen push from BLE)
  QueueHandle_t incomingCallQueue = nullptr;
  
  // --- PSRAM Optimization: Static Queue Buffers ---
  uint8_t* q_command_storage = nullptr;
  StaticQueue_t* q_command_struct = nullptr;

  uint8_t* q_quick_storage = nullptr;
  StaticQueue_t* q_quick_struct = nullptr;

  uint8_t* q_add_storage = nullptr;
  StaticQueue_t* q_add_struct = nullptr;

  uint8_t* q_call_storage = nullptr;
  StaticQueue_t* q_call_struct = nullptr;
  // ------------------------------------------------

  // Quick notification timer tracking
  unsigned long quickNotificationShowTime = 0;
  bool quickNotificationActive = false;

  // FIX: Set when the call screen is pushed while a quick notification was visible.
  // Rather than pop-then-push (which routes through the base screen and confuses EEZ
  // flow's event system, causing action_accept_call to double-fire), we push the call
  // screen directly on top and cancel the timer. On call end we pop twice to clean up
  // the buried quick notification screen.
  bool quickNotificationBuriedUnderCall = false;

  unsigned long startupTime = 0;
  static const unsigned long STARTUP_QUIET_PERIOD_MS = 3000;
};

#endif