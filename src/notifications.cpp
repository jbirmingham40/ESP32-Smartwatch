#include "core/lv_disp.h"
#include "notifications.h"
#include "src/eez-flow.h"
#include "src/structs.h"
#include "src/actions.h"
#include "src/screens.h"
#include "src/vars.h"
#include "ble.h"

// Global instance
extern NotificationStore notificationStore;
extern BLE ble;

char active_call_duration[10] = { 0 };
time_t callStart;


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

// CRITICAL: Queue validation to prevent FreeRTOS internal crashes
#define VALIDATE_QUEUE(queue, name) \
  do { \
    if (!(queue)) { \
      Serial.printf("ERROR: Queue '%s' is NULL at %s:%d\n", name, __FILE__, __LINE__); \
      return; \
    } \
    /* Check queue control structure isn't corrupted */ \
    QueueHandle_t q = (queue); \
    if ((void*)q == (void*)0xFFFFFFFF) { \
      Serial.printf("ERROR: Queue '%s' is poisoned (0xFFFFFFFF) at %s:%d\n", name, __FILE__, __LINE__); \
      return; \
    } \
  } while(0)

// ============================================================================
// CRITICAL FIX: Helper struct and callbacks for async screen operations
// All LVGL screen operations MUST use lv_async_call() to run on LVGL thread
// ============================================================================
struct ScreenOperation {
  uint8_t screenId;
  lv_scr_load_anim_t animType;
  uint16_t time;
  uint16_t delay;
  bool isPush;  // true = push, false = pop
};

// Async callbacks that run on LVGL thread
static void asyncPushScreen(void* param) {
  ScreenOperation* op = (ScreenOperation*)param;
  if (op) {
    eez_flow_push_screen(op->screenId, op->animType, op->time, op->delay);
    lv_mem_free(op);
  }
}

static void asyncPopScreen(void* param) {
  ScreenOperation* op = (ScreenOperation*)param;
  if (op) {
    eez_flow_pop_screen(op->animType, op->time, op->delay);
    lv_mem_free(op);
  }
}

void action_update_call_duration(lv_event_t *e) {
  time_t now = ::millis();
  time_t delta = now-callStart;

  uint16_t hours = delta / 3600000;
  uint16_t minutes = (delta % 3600000) / 60000;
  uint16_t seconds = (delta % 60000) / 1000;
  
  snprintf(active_call_duration, sizeof(active_call_duration), "%02d:%02d:%02d", hours, minutes, seconds);
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ACTIVE_CALL_DURATION, eez::StringValue(active_call_duration));
}

void set_var_active_call_duration(const char *value) {
}


void action_dismiss_quick_notification(lv_event_t* e) {
  notificationStore.dismissQuickNotification();
}

void action_accept_call(lv_event_t* e) {
  eez::Value uidValue = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_UID);
  uint32_t uid = uidValue.getUInt32();

  if (uid == 0) {
    Serial.println(">> Accept call: No valid UID");
    return;
  }

  // CRITICAL: Set callInProgress TRUE and show the icon BEFORE sendAction().
  // sendAction() triggers a BLE write after which iOS immediately fires eventID==2
  // on the ringing UID. The BLE Core 0 callback checks isCallInProgress() to decide
  // whether to show "Missed Call" — the flag must be true before that can fire.
  eez::Value nameValue   = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_NAME);
  eez::Value numberValue = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_NUMBER);
  strncpy(notificationStore.activeCallCallerName, nameValue.getString(), sizeof(notificationStore.activeCallCallerName) - 1);
  notificationStore.activeCallCallerName[sizeof(notificationStore.activeCallCallerName) - 1] = '\0';
  strncpy(notificationStore.activeCallCallerNumber, numberValue.getString(), sizeof(notificationStore.activeCallCallerNumber) - 1);
  notificationStore.activeCallCallerNumber[sizeof(notificationStore.activeCallCallerNumber) - 1] = '\0';
  notificationStore.activeCallUid = uid;
  notificationStore.callInProgress.store(true);  // MUST be before sendAction()
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ACTIVE_CALL_VISIBLE, eez::BooleanValue(true));
  callStart = ::millis();

  ble.sendAction(uid, true);

  // Do NOT pop the screen. The call screen stays visible so the user can see the call
  // is active and tap "End Call". The screen is dismissed in CLEAR_ACTIVE_CALL when
  // iOS confirms the call has truly ended.
  Serial.println(">> Call accepted — keeping call screen visible");
}

void action_decline_call(lv_event_t* e) {
  eez::Value uidValue = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_UID);
  uint32_t uid = uidValue.getUInt32();

  if (uid == 0) {
    Serial.println(">> Decline call: No valid UID");
    return;
  }

  ble.sendAction(uid, false);

  // CRITICAL FIX: Use async screen pop
  ScreenOperation* op = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
  if (op) {
    op->animType = LV_SCR_LOAD_ANIM_FADE_OUT;
    op->time = 200;
    op->delay = 0;
    op->isPush = false;
    lv_async_call(asyncPopScreen, op);
  }
}

// Tapping the active-call icon re-opens the call screen.
// Bind this to the onClick of your active-call icon widget in EEZ Studio.
void action_return_to_call(lv_event_t* e) {
  if (!notificationStore.isCallInProgress()) {
    Serial.println(">> return_to_call: no active call, ignoring");
    return;
  }
  notificationStore.queueReturnToCall();
}

void action_next_notification(lv_event_t* e) {
  notificationStore.showNext();
}

void action_prev_notification(lv_event_t* e) {
  notificationStore.showPrevious();
}

void action_accept_notification(lv_event_t* e) {
  uint32_t uid = notificationStore.getCurrentUID();
  if (uid == 0) {
    Serial.println(">> Accept: No valid notification");
    return;
  }

  if (!notificationStore.currentHasPositiveAction()) {
    Serial.println(">> Accept: Notification has no positive action");
    return;
  }

  ble.sendAction(uid, true);
}

void action_dismiss_notification(lv_event_t* e) {
  uint32_t uid = notificationStore.getCurrentUID();
  if (uid == 0) {
    Serial.println(">> Dismiss: No valid notification");
    return;
  }

  if (!notificationStore.currentHasNegativeAction()) {
    Serial.println(">> Dismiss: Notification has no negative action");
    return;
  }

  ble.sendAction(uid, false);
}

void action_load_notifications(lv_event_t* e) {
  notificationStore.showLatest();
}

void NotificationStore::showQuickNotification(const QuickNotificationData& data) {
  // Static buffer for merged title - avoids heap fragmentation
  static char mergedTitle[260];

  // SAFETY CHECK: Verify LVGL is in a good state before screen operations
  lv_disp_t* disp = lv_disp_get_default();
  if (!disp) {
    Serial.println(">> ERROR: LVGL display is NULL, skipping quick notification");
    return;
  }

  lv_obj_t* actScr = lv_scr_act();
  if (!actScr) {
    Serial.println(">> ERROR: LVGL active screen is NULL, skipping quick notification");
    return;
  }

  bool needToPush = !quickNotificationActive;

  if (needToPush) {
    quickNotificationActive = true;  // Set BEFORE async call to prevent double-push

    // CRITICAL FIX: Use lv_async_call for screen push
    ScreenOperation* op = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
    if (op) {
      op->screenId = SCREEN_ID_QUICK_NOTIFICATION;
      op->animType = LV_SCR_LOAD_ANIM_OVER_BOTTOM;
      op->time = 150;
      op->delay = 0;
      op->isPush = true;
      lv_async_call(asyncPushScreen, op);
    } else {
      Serial.println(">> ERROR: Failed to allocate screen operation");
      quickNotificationActive = false;
      return;
    }
  }

  // Now update the content (these are safe to call directly)
  const char* icon = data.icon[0] ? data.icon : "";
  const char* message = data.message[0] ? data.message : "";

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_QUICK_NOTIFICATION_ICON, eez::StringValue(icon));

  bool hasTitle = (data.title[0] != '\0');
  bool hasSubtitle = (data.subtitle[0] != '\0');

  if (hasTitle && hasSubtitle) {
    snprintf(mergedTitle, sizeof(mergedTitle), "%s - %s", data.title, data.subtitle);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_QUICK_NOTIFICATION_TITLE, eez::StringValue(mergedTitle));
  } else if (hasTitle) {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_QUICK_NOTIFICATION_TITLE, eez::StringValue(data.title));
  } else if (hasSubtitle) {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_QUICK_NOTIFICATION_TITLE, eez::StringValue(data.subtitle));
  } else {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_QUICK_NOTIFICATION_TITLE, eez::StringValue(""));
  }

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_QUICK_NOTIFICATION_MESSAGE, eez::StringValue(message));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_QUICK_NOTIFICATION_IMPORTANT, eez::BooleanValue(data.important));

  // Reset the timer
  quickNotificationShowTime = ::millis();
}

void NotificationStore::dismissQuickNotification() {
  if (!quickNotificationActive) {
    return;  // Already inactive, nothing to do
  }

  // Ensure minimum display time
  unsigned long elapsed = ::millis() - quickNotificationShowTime;
  if (elapsed < 300) {
    return;  // Too early to dismiss
  }

  // Verify LVGL is in a good state
  lv_disp_t* disp = lv_disp_get_default();
  lv_obj_t* actScr = lv_scr_act();

  if (!disp || !actScr) {
    quickNotificationActive = false;
    quickNotificationShowTime = 0;
    return;
  }

  quickNotificationActive = false;
  quickNotificationShowTime = 0;

  // CRITICAL FIX: Use lv_async_call for screen pop
  ScreenOperation* op = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
  if (op) {
    op->animType = LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
    op->time = 150;
    op->delay = 0;
    op->isPush = false;
    lv_async_call(asyncPopScreen, op);
  } else {
    Serial.println(">> ERROR: Failed to allocate screen operation");
  }
}

void NotificationStore::queueQuickNotification(const char* icon, const char* title, const char* subtitle, const char* message, bool important) {
  if (!quickNotificationQueue) {
    Serial.println(">> quickNotificationQueue is NULL!");
    return;
  }

  QuickNotificationData data;
  strncpy(data.icon, icon ? icon : "", sizeof(data.icon) - 1);
  data.icon[sizeof(data.icon) - 1] = '\0';
  strncpy(data.title, title ? title : "", sizeof(data.title) - 1);
  data.title[sizeof(data.title) - 1] = '\0';
  strncpy(data.subtitle, subtitle ? subtitle : "", sizeof(data.subtitle) - 1);
  data.subtitle[sizeof(data.subtitle) - 1] = '\0';
  strncpy(data.message, message ? message : "", sizeof(data.message) - 1);
  data.message[sizeof(data.message) - 1] = '\0';
  data.important = important;

  if (xQueueSend(quickNotificationQueue, &data, 0) != pdTRUE) {
    Serial.println(">> Quick notification queue full, skipping");
    return;
  }

  // Queue a command to trigger processing
  NotificationCommandData cmd;
  cmd.cmd = NotificationCommand::SHOW_QUICK_NOTIFICATION;
  cmd.uid = 0;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println(">> Failed to queue quick notification command");
  }
}

void NotificationStore::queueAddNotification(
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
  const char* negativeActionLabel) {
  // CRITICAL: Validate queues before attempting to use them
  // This is called from Core 0 (BLE) and must be extra careful
  VALIDATE_QUEUE(addNotificationQueue, "addNotificationQueue in queueAdd");
  VALIDATE_QUEUE(commandQueue, "commandQueue in queueAdd");

  // CRITICAL FIX: Check queue space BEFORE preparing data
  UBaseType_t spaces = uxQueueSpacesAvailable(addNotificationQueue);
  if (spaces == 0) {
    Serial.println(">> ERROR: Add notification queue FULL! Dropping notification");
    Serial.printf(">> Dropped notification UID: %u, Title: %s\n", uid, title ? title : "(null)");
    return;
  }
  if (spaces <= 2) {
    Serial.printf(">> WARNING: Add notification queue low (%d slots remaining)\n", spaces);
  }

  AddNotificationData data;
  strncpy(data.appId, appId ? appId : "", sizeof(data.appId) - 1);
  data.appId[sizeof(data.appId) - 1] = '\0';
  strncpy(data.title, title ? title : "", sizeof(data.title) - 1);
  data.title[sizeof(data.title) - 1] = '\0';
  strncpy(data.subtitle, subtitle ? subtitle : "", sizeof(data.subtitle) - 1);
  data.subtitle[sizeof(data.subtitle) - 1] = '\0';
  strncpy(data.message, message ? message : "", sizeof(data.message) - 1);
  data.message[sizeof(data.message) - 1] = '\0';
  strncpy(data.dateTime, dateTime ? dateTime : "", sizeof(data.dateTime) - 1);
  data.dateTime[sizeof(data.dateTime) - 1] = '\0';
  strncpy(data.positiveActionLabel, positiveActionLabel ? positiveActionLabel : "", sizeof(data.positiveActionLabel) - 1);
  data.positiveActionLabel[sizeof(data.positiveActionLabel) - 1] = '\0';
  strncpy(data.negativeActionLabel, negativeActionLabel ? negativeActionLabel : "", sizeof(data.negativeActionLabel) - 1);
  data.negativeActionLabel[sizeof(data.negativeActionLabel) - 1] = '\0';
  data.uid = uid;
  data.categoryId = categoryId;
  data.important = important;
  data.hasPositiveAction = hasPositiveAction;
  data.hasNegativeAction = hasNegativeAction;

  if (xQueueSend(addNotificationQueue, &data, 0) != pdTRUE) {
    Serial.println(">> ERROR: Failed to queue add notification data (unexpected failure)");
    return;
  }

  // Queue a command to trigger processing
  NotificationCommandData cmd;
  cmd.cmd = NotificationCommand::ADD_NOTIFICATION;
  cmd.uid = uid;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println(">> ERROR: Failed to queue add notification command");
  } else {
    Serial.printf(">> Queued ADD_NOTIFICATION for UID %u\n", uid);
  }
}

bool NotificationStore::begin() {
  // Check if already initialized
  if (initialized.exchange(true)) {
    return true;
  }

  startupTime = ::millis();

  // CRITICAL FIX: Create mutex FIRST before any operations
  notificationsMutex = xSemaphoreCreateMutex();
  if (!notificationsMutex) {
    Serial.println(">> FATAL: Failed to create notifications mutex!");
    initialized = false;
    return false;
  }

  // Reset quick notification state
  quickNotificationActive = false;
  quickNotificationShowTime = 0;

  // ----------------------------------------------------------------------
  // MEMORY OPTIMIZATION: Move Queues to PSRAM
  // CRITICAL FIX: Increased queue sizes to prevent overflow
  // ----------------------------------------------------------------------

  // 1. Command Queue - INCREASED from 16 to 20
  size_t cmdItemSize = sizeof(NotificationCommandData);
  size_t cmdQueueLen = 20;  // CRITICAL FIX: Was 16
  q_command_storage = (uint8_t*)heap_caps_malloc(cmdQueueLen * cmdItemSize, MALLOC_CAP_SPIRAM);
  q_command_struct = (StaticQueue_t*)heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL);  // Struct in internal RAM is safer

  if (q_command_storage && q_command_struct) {
    commandQueue = xQueueCreateStatic(cmdQueueLen, cmdItemSize, q_command_storage, q_command_struct);
  }

  if (!commandQueue) {
    Serial.println(">> FATAL: Failed to create notification command queue!");
    vSemaphoreDelete(notificationsMutex);  // Clean up mutex
    notificationsMutex = nullptr;
    initialized = false;
    return false;
  }

  // 2. Quick Notification Queue (~2.2KB) - INCREASED from 4 to 10
  size_t quickItemSize = sizeof(QuickNotificationData);
  size_t quickQueueLen = 10;  // CRITICAL FIX: Was 4
  q_quick_storage = (uint8_t*)heap_caps_malloc(quickQueueLen * quickItemSize, MALLOC_CAP_SPIRAM);
  q_quick_struct = (StaticQueue_t*)heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL);

  if (q_quick_storage && q_quick_struct) {
    quickNotificationQueue = xQueueCreateStatic(quickQueueLen, quickItemSize, q_quick_storage, q_quick_struct);
  }

  if (!quickNotificationQueue) {
    Serial.println(">> FATAL: Failed to create quick notification queue!");
    vSemaphoreDelete(notificationsMutex);
    notificationsMutex = nullptr;
    initialized = false;
    return false;
  }

  // 3. Add Notification Queue (~6KB) - INCREASED from 8 to 15
  size_t addItemSize = sizeof(AddNotificationData);
  size_t addQueueLen = 15;  // CRITICAL FIX: Was 8
  q_add_storage = (uint8_t*)heap_caps_malloc(addQueueLen * addItemSize, MALLOC_CAP_SPIRAM);
  q_add_struct = (StaticQueue_t*)heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL);

  if (q_add_storage && q_add_struct) {
    addNotificationQueue = xQueueCreateStatic(addQueueLen, addItemSize, q_add_storage, q_add_struct);
  }

  if (!addNotificationQueue) {
    Serial.println(">> FATAL: Failed to create add notification queue!");
    vSemaphoreDelete(notificationsMutex);
    notificationsMutex = nullptr;
    initialized = false;
    return false;
  }

  // 4. Incoming Call Queue - INCREASED from 2 to 5
  size_t callItemSize = sizeof(IncomingCallData);
  size_t callQueueLen = 5;  // CRITICAL FIX: Was 2
  q_call_storage = (uint8_t*)heap_caps_malloc(callQueueLen * callItemSize, MALLOC_CAP_SPIRAM);
  q_call_struct = (StaticQueue_t*)heap_caps_malloc(sizeof(StaticQueue_t), MALLOC_CAP_INTERNAL);

  if (q_call_storage && q_call_struct) {
    incomingCallQueue = xQueueCreateStatic(callQueueLen, callItemSize, q_call_storage, q_call_struct);
  }

  if (!incomingCallQueue) {
    Serial.println(">> FATAL: Failed to create incoming call queue!");
    vSemaphoreDelete(notificationsMutex);
    notificationsMutex = nullptr;
    initialized = false;
    return false;
  }

  // Allocate notification storage in PSRAM (Already correct in original code)
  notifications = (StoredNotification*)heap_caps_malloc(
      sizeof(StoredNotification) * MAX_NOTIFICATIONS, 
      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
  );    

  if (!notifications) {
    Serial.println(">> FATAL: Failed to allocate notification storage in PSRAM!");
    vSemaphoreDelete(notificationsMutex);
    notificationsMutex = nullptr;
    initialized = false;
    return false;
  }

  // Initialize all slots
  for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
    notifications[i].valid = false;
    notifications[i].viewed = false;
    notifications[i].iconId[0] = '\0';
    notifications[i].title[0] = '\0';
    notifications[i].subtitle[0] = '\0';
    notifications[i].message[0] = '\0';
    notifications[i].dateTime = 0;
    notifications[i].important = false;
    notifications[i].hasNegativeAction = false;
    notifications[i].hasPositiveAction = false;
  }

  totalCount = 0;
  currentIndex = -1;
  newestIndex = -1;

  Serial.printf(">> NotificationStore: Allocated %d slots in PSRAM (%d bytes)\n",
                MAX_NOTIFICATIONS, sizeof(StoredNotification) * MAX_NOTIFICATIONS);
  Serial.printf(">> Queue sizes - CMD:%d QUICK:%d ADD:%d CALL:%d\n",
                cmdQueueLen, quickQueueLen, addQueueLen, callQueueLen);

  updateDisplay();

  return true;
}

void NotificationStore::queueRemove(uint32_t uid) {
  if (!commandQueue) return;

  NotificationCommandData cmd;
  cmd.cmd = NotificationCommand::REMOVE;
  cmd.uid = uid;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println(">> Warning: Command queue full, remove dropped");
  }
}

void NotificationStore::processCommands() {
  // CRITICAL: Validate object state
  VALIDATE_PTR(this, "NotificationStore object");
  VALIDATE_PTR(notifications, "notifications array");
  
  // CRITICAL: Validate all queues before processing
  VALIDATE_QUEUE(commandQueue, "commandQueue");
  VALIDATE_QUEUE(quickNotificationQueue, "quickNotificationQueue");
  VALIDATE_QUEUE(addNotificationQueue, "addNotificationQueue");
  VALIDATE_QUEUE(incomingCallQueue, "incomingCallQueue");

  // Use static buffers to avoid stack overflow - these are only accessed from Core 1
  static NotificationCommandData cmd;
  static QuickNotificationData qnData;
  static AddNotificationData addData;
  static IncomingCallData callData;

  // Helper lambda to check LVGL state before screen operations
  auto isLvglReady = []() -> bool {
    lv_disp_t* disp = lv_disp_get_default();
    if (!disp) {
      Serial.println(">> ERROR: LVGL display is NULL");
      return false;
    }
    lv_obj_t* actScr = lv_scr_act();
    if (!actScr) {
      Serial.println(">> ERROR: LVGL active screen is NULL");
      return false;
    }
    return true;
  };

  while (xQueueReceive(commandQueue, &cmd, 0) == pdTRUE) {
    // Debug output removed to prevent Serial buffer overflow

    switch (cmd.cmd) {
      case NotificationCommand::REMOVE:
        removeNotification(cmd.uid);
        break;

      case NotificationCommand::SHOW_NEXT:
        showNext();
        break;

      case NotificationCommand::SHOW_PREV:
        showPrevious();
        break;

      case NotificationCommand::SHOW_LATEST:
        showLatest();
        break;

      case NotificationCommand::POP_CALL_SCREEN:
        if (isLvglReady()) {
          Serial.println(">> Queueing async pop of call screen");
          ScreenOperation* op = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
          if (op) {
            op->animType = LV_SCR_LOAD_ANIM_FADE_OUT;
            op->time = 200;
            op->delay = 0;
            op->isPush = false;
            lv_async_call(asyncPopScreen, op);
          }
          // FIX: If the call screen was pushed on top of a buried quick notification,
          // pop that stale quick notification screen too so the base screen is restored.
          if (quickNotificationBuriedUnderCall) {
            Serial.println(">> Also popping buried quick notification screen");
            quickNotificationBuriedUnderCall = false;
            ScreenOperation* op2 = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
            if (op2) {
              op2->animType = LV_SCR_LOAD_ANIM_NONE;
              op2->time = 0;
              op2->delay = 250;  // After the call screen fade-out completes
              op2->isPush = false;
              lv_async_call(asyncPopScreen, op2);
            }
          }
        }
        break;

      case NotificationCommand::PUSH_CALL_SCREEN:
        {
          Serial.println(">> Received PUSH_CALL_SCREEN command");
          if (!incomingCallQueue) {
            Serial.println(">> ERROR: incomingCallQueue is NULL!");
            break;
          }
          if (xQueueReceive(incomingCallQueue, &callData, 0) == pdTRUE) {
            if (!isLvglReady()) {
              Serial.println(">> Skipping call screen push - LVGL not ready");
              break;
            }
            Serial.printf(">> Processing incoming call: %s (UID %u)\n", callData.callerName, callData.uid);

            // FIX: If a quick notification is currently visible, cancel its timer state
            // and push the call screen directly ON TOP of it — do NOT pop it first.
            // Popping and re-pushing routes through the base screen momentarily, causing
            // EEZ flow to fire "screen appeared" events on the base screen. Those events
            // reset state that action_accept_call depends on, making the accept button
            // appear to do nothing (and triggering a second spurious accept with the
            // new active-call UID). By burying the quick notification under the call
            // screen we avoid that EEZ flow routing entirely. We remember to do a
            // double-pop when the call ends to clean up the buried screen.
            if (quickNotificationActive) {
              Serial.println(">> Quick notification buried under call screen (will double-pop on call end)");
              quickNotificationActive = false;
              quickNotificationShowTime = 0;
              quickNotificationBuriedUnderCall = true;
            }

            // Set variables BEFORE pushing screen
            eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_NAME,
                                         eez::StringValue(callData.callerName[0] ? callData.callerName : "Unknown Caller"));
            eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_NUMBER,
                                         eez::StringValue(callData.callerNumber));
            eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_UID,
                                         eez::IntegerValue(callData.uid));

            Serial.println(">> Queueing async push of call screen");
            ScreenOperation* op = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
            if (op) {
              op->screenId = SCREEN_ID_INCOMING_CALL;
              op->animType = LV_SCR_LOAD_ANIM_FADE_IN;
              op->time = 200;
              op->delay = 0;
              op->isPush = true;
              lv_async_call(asyncPushScreen, op);
            }
          } else {
            Serial.println(">> ERROR: No data in incomingCallQueue!");
          }
        }
        break;

      case NotificationCommand::SHOW_QUICK_NOTIFICATION:
        {
          Serial.println(">> Received SHOW_QUICK_NOTIFICATION command");
          if (!quickNotificationQueue) {
            Serial.println(">> ERROR: quickNotificationQueue is NULL!");
            break;
          }
          if (xQueueReceive(quickNotificationQueue, &qnData, 0) == pdTRUE) {
            Serial.printf(">> Got quick notification data: %s\n", qnData.title);
            showQuickNotification(qnData);
          } else {
            Serial.println(">> ERROR: No data in quickNotificationQueue!");
          }
        }
        break;

      case NotificationCommand::ADD_NOTIFICATION:
        {
          Serial.println(">> Received ADD_NOTIFICATION command");
          if (!addNotificationQueue) {
            Serial.println(">> ERROR: addNotificationQueue is NULL!");
            break;
          }
          if (xQueueReceive(addNotificationQueue, &addData, 0) == pdTRUE) {
            Serial.printf(">> Processing queued notification: %s (UID %u)\n", addData.title, addData.uid);
            addNotification(
              addData.appId,
              addData.title,
              addData.subtitle,
              addData.message,
              addData.dateTime,
              addData.uid,
              addData.categoryId,
              addData.important,
              addData.hasPositiveAction,
              addData.hasNegativeAction,
              addData.positiveActionLabel,
              addData.negativeActionLabel);
          } else {
            Serial.println(">> ERROR: No data in addNotificationQueue!");
          }
        }
        break;

      // Call truly ended — pop the call screen and hide the active-call icon
      case NotificationCommand::CLEAR_ACTIVE_CALL:
        {
          Serial.println(">> CLEAR_ACTIVE_CALL: call ended, dismissing call screen and hiding icon");
          callInProgress.store(false);
          activeCallUid = 0;
          activeCallCallerName[0] = '\0';
          activeCallCallerNumber[0] = '\0';
          eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ACTIVE_CALL_VISIBLE, eez::BooleanValue(false));
          if (isLvglReady()) {
            ScreenOperation* op = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
            if (op) {
              op->animType = LV_SCR_LOAD_ANIM_FADE_OUT;
              op->time = 300;
              op->delay = 0;
              op->isPush = false;
              lv_async_call(asyncPopScreen, op);
            }
            // FIX: If the call screen was pushed on top of a buried quick notification,
            // pop that stale quick notification screen too so the base screen is restored.
            if (quickNotificationBuriedUnderCall) {
              Serial.println(">> Also popping buried quick notification screen after call end");
              quickNotificationBuriedUnderCall = false;
              ScreenOperation* op2 = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
              if (op2) {
                op2->animType = LV_SCR_LOAD_ANIM_NONE;
                op2->time = 0;
                op2->delay = 350;  // After the call screen fade-out (300ms) completes
                op2->isPush = false;
                lv_async_call(asyncPopScreen, op2);
              }
            }
          }
        }
        break;

      // User tapped the active-call icon — re-push the call screen
      case NotificationCommand::RETURN_TO_CALL:
        {
          Serial.println(">> RETURN_TO_CALL: re-pushing call screen");
          if (!callInProgress.load()) {
            Serial.println(">> RETURN_TO_CALL: no active call, ignoring");
            break;
          }
          if (!isLvglReady()) {
            Serial.println(">> RETURN_TO_CALL: LVGL not ready, ignoring");
            break;
          }
          eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_NAME,
                                       eez::StringValue(activeCallCallerName[0] ? activeCallCallerName : "Active Call"));
          eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_NUMBER,
                                       eez::StringValue(activeCallCallerNumber));
          eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_UID,
                                       eez::IntegerValue(activeCallUid));
          ScreenOperation* op = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
          if (op) {
            op->screenId = SCREEN_ID_INCOMING_CALL;
            op->animType = LV_SCR_LOAD_ANIM_FADE_IN;
            op->time = 200;
            op->delay = 0;
            op->isPush = true;
            lv_async_call(asyncPushScreen, op);
          }
        }
        break;

      // iOS sent the replacement "Active Call" UID — update INCOMING_CALL_UID so that
      // "End Call" (action_decline_call) targets the correct live UID
      case NotificationCommand::SET_ACTIVE_CALL_UID:
        Serial.printf(">> SET_ACTIVE_CALL_UID: updating call UID to %u\n", cmd.uid);
        activeCallUid = cmd.uid;
        eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_INCOMING_CALL_UID,
                                     eez::IntegerValue(cmd.uid));
        break;
    }
  }

  // Check if quick notification should auto-dismiss
  if (quickNotificationActive) {
    unsigned long now = ::millis();
    unsigned long elapsed = now - quickNotificationShowTime;

    if (elapsed >= QUICK_NOTIFICATION_DURATION_MS) {
      // Verify LVGL is in a good state before popping
      lv_disp_t* disp = lv_disp_get_default();
      lv_obj_t* actScr = lv_scr_act();

      if (disp && actScr) {
        Serial.printf(">> Quick notification timer expired (elapsed=%lu ms)\n", elapsed);
        quickNotificationActive = false;
        quickNotificationShowTime = 0;

        // CRITICAL FIX: Use lv_async_call for auto-dismiss
        ScreenOperation* op = (ScreenOperation*)lv_mem_alloc(sizeof(ScreenOperation));
        if (op) {
          op->animType = LV_SCR_LOAD_ANIM_MOVE_BOTTOM;
          op->time = 150;
          op->delay = 0;
          op->isPush = false;
          lv_async_call(asyncPopScreen, op);
        }
      } else {
        Serial.println(">> Quick notification timer expired but LVGL not ready, resetting state");
        quickNotificationActive = false;
        quickNotificationShowTime = 0;
      }
    }
  }
}

void NotificationStore::queuePopCallScreen() {
  if (!commandQueue) return;

  NotificationCommandData cmd;
  cmd.cmd = NotificationCommand::POP_CALL_SCREEN;
  cmd.uid = 0;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println(">> Warning: Command queue full, pop screen dropped");
  }
}

void NotificationStore::queueClearActiveCall() {
  if (!commandQueue) return;

  NotificationCommandData cmd;
  cmd.cmd = NotificationCommand::CLEAR_ACTIVE_CALL;
  cmd.uid = 0;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println(">> Warning: Command queue full, CLEAR_ACTIVE_CALL dropped");
  }
}

void NotificationStore::queueReturnToCall() {
  if (!commandQueue) return;

  NotificationCommandData cmd;
  cmd.cmd = NotificationCommand::RETURN_TO_CALL;
  cmd.uid = 0;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println(">> Warning: Command queue full, RETURN_TO_CALL dropped");
  }
}

void NotificationStore::queueSetActiveCallUid(uint32_t uid) {
  if (!commandQueue) return;

  NotificationCommandData cmd;
  cmd.cmd = NotificationCommand::SET_ACTIVE_CALL_UID;
  cmd.uid = uid;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println(">> Warning: Command queue full, SET_ACTIVE_CALL_UID dropped");
  }
}

void NotificationStore::queuePushCallScreen(const char* callerName, const char* callerNumber, uint32_t uid) {
  if (!incomingCallQueue || !commandQueue) {
    Serial.println(">> queuePushCallScreen: Queue is NULL!");
    return;
  }

  IncomingCallData data;
  strncpy(data.callerName, callerName ? callerName : "", sizeof(data.callerName) - 1);
  data.callerName[sizeof(data.callerName) - 1] = '\0';
  strncpy(data.callerNumber, callerNumber ? callerNumber : "", sizeof(data.callerNumber) - 1);
  data.callerNumber[sizeof(data.callerNumber) - 1] = '\0';
  data.uid = uid;

  if (xQueueSend(incomingCallQueue, &data, 0) != pdTRUE) {
    Serial.println(">> Incoming call queue full, skipping");
    return;
  }

  // Queue a command to trigger processing
  NotificationCommandData cmd;
  cmd.cmd = NotificationCommand::PUSH_CALL_SCREEN;
  cmd.uid = uid;

  if (xQueueSend(commandQueue, &cmd, 0) != pdTRUE) {
    Serial.println(">> Failed to queue push call screen command");
  } else {
    Serial.printf(">> Queued PUSH_CALL_SCREEN for UID %u\n", uid);
  }
}

void NotificationStore::sanitizeString(char* dest, const char* src, size_t maxLen) {
  // CRITICAL: Check for NULL and poison values
  if (!src || !dest || maxLen == 0 || 
      (void*)src == (void*)0xFFFFFFFF || (void*)dest == (void*)0xFFFFFFFF) {
    if (dest && maxLen > 0 && (void*)dest != (void*)0xFFFFFFFF) {
      dest[0] = '\0';
    }
    return;
  }

  size_t j = 0;
  size_t srcLen = strlen(src);

  for (size_t i = 0; i < srcLen && j < maxLen - 1;) {
    uint8_t c = (uint8_t)src[i];

    // Regular ASCII (0x00-0x7F) - copy directly
    if (c < 0x80) {
      dest[j++] = src[i++];
      continue;
    }

    // Check for UTF-8 multi-byte sequences we want to replace
    if (i + 2 < srcLen && c == 0xE2 && (uint8_t)src[i + 1] == 0x80) {
      uint8_t c3 = (uint8_t)src[i + 2];

      if (c3 == 0x94 || c3 == 0x93) {  // Em dash or en dash
        dest[j++] = '-';
        i += 3;
        continue;
      }
      if (c3 == 0x9C || c3 == 0x9D) {  // Smart double quotes
        dest[j++] = '"';
        i += 3;
        continue;
      }
      if (c3 == 0x98 || c3 == 0x99) {  // Smart single quotes
        dest[j++] = '\'';
        i += 3;
        continue;
      }
      if (c3 == 0xA6) {  // Ellipsis
        if (j + 3 < maxLen - 1) {
          dest[j++] = '.';
          dest[j++] = '.';
          dest[j++] = '.';
        }
        i += 3;
        continue;
      }
      if (c3 == 0xA2) {  // Bullet
        dest[j++] = '*';
        i += 3;
        continue;
      }
    }

    // Unhandled UTF-8 sequence - determine length and skip/replace
    int seqLen = 1;
    if ((c & 0xE0) == 0xC0) {
      seqLen = 2;
    } else if ((c & 0xF0) == 0xE0) {
      seqLen = 3;
    } else if ((c & 0xF8) == 0xF0) {
      seqLen = 4;
    }

    i += seqLen;
  }

  dest[j] = '\0';
}

void NotificationStore::setIconFromAppId(const char* appId, uint8_t categoryId, char* iconId, size_t maxLen) {
  // CRITICAL: Validate output pointer
  VALIDATE_PTR(iconId, "iconId in setIconFromAppId");
  
  const char* icon = "notification-generic";

  // CRITICAL: Validate appId before using strstr (can crash on invalid pointer)
  if (appId && (void*)appId != (void*)0xFFFFFFFF && strlen(appId) > 0) {
    if (strstr(appId, "ring")) icon = "notification-ring";
    else if (strstr(appId, "460CE946C1BE4A3AA55C026D2BEFBCA5")) icon = "notification-dogdetector";
    else if (strstr(appId, "calendar")) icon = "notification-calendar";
    else if (strstr(appId, "myq")) icon = "notification-myq";
    else if (strstr(appId, "Amazon")) icon = "notification-amazon";
    else if (strstr(appId, "mail")) icon = "notification-mail";
    else if (strstr(appId, "fantasyFootball")) icon = "notification-fantasyfootball";
    else if (strstr(appId, "MobileSMS")) icon = "notification-imessage";
    else if (strstr(appId, "uber")) icon = "notification-uber";
    else if (strstr(appId, "chatlyio")) icon = "notification-slack";
    else if (strstr(appId, "rachio")) icon = "notification-rachio";
    else if (strstr(appId, "reo.link")) icon = "notification-reolink";
    else if (strstr(appId, "pandora")) icon = "notification-pandora-80";
    else if (strstr(appId, "com.apple.weather")) icon = "notification-weather-80";
    else if (strstr(appId, "spotify")) icon = "notification-spotify-80";
    else if (strstr(appId, "bambulab")) icon = "notification-bambulab-80";
  }

  if (strcmp(icon, "notification-generic") == 0) {
    switch (categoryId) {
      case 1: icon = "notification-incomingcall"; break;
      case 2: icon = "notification-missedcall"; break;
      case 3: icon = "notification-voicemail"; break;
      case 5: icon = "notification-calendar"; break;
      case 6: icon = "notification-mail"; break;
      default: Serial.printf("Unknown ANCS application name %s\n", appId);
    }
  }

  strncpy(iconId, icon, maxLen - 1);
  iconId[maxLen - 1] = '\0';
}

bool NotificationStore::addNotification(
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
  const char* negativeActionLabel) {

  if (!initialized.load() || !notifications) {
    Serial.println(">> NotificationStore not initialized!");
    return false;
  }

  // CRITICAL: Validate all string parameters to prevent crashes in string operations
  // Check for poison value (0xFFFFFFFF) which causes crashes at NULL+offset
  if ((void*)appId == (void*)0xFFFFFFFF) {
    Serial.println("ERROR: appId is poison value");
    appId = nullptr;
  }
  if ((void*)title == (void*)0xFFFFFFFF) {
    Serial.println("ERROR: title is poison value");
    title = nullptr;
  }
  if ((void*)subtitle == (void*)0xFFFFFFFF) {
    Serial.println("ERROR: subtitle is poison value");
    subtitle = nullptr;
  }
  if ((void*)message == (void*)0xFFFFFFFF) {
    Serial.println("ERROR: message is poison value");
    message = nullptr;
  }
  if ((void*)dateTime == (void*)0xFFFFFFFF) {
    Serial.println("ERROR: dateTime is poison value");
    dateTime = nullptr;
  }
  if ((void*)positiveActionLabel == (void*)0xFFFFFFFF) {
    Serial.println("ERROR: positiveActionLabel is poison value");
    positiveActionLabel = nullptr;
  }
  if ((void*)negativeActionLabel == (void*)0xFFFFFFFF) {
    Serial.println("ERROR: negativeActionLabel is poison value");
    negativeActionLabel = nullptr;
  }

  int existingSlot = -1;
  int maxCheck = (totalCount.load() > MAX_NOTIFICATIONS) ? MAX_NOTIFICATIONS : totalCount.load();
  for (int i = 0; i < maxCheck; i++) {
    if (notifications[i].valid && notifications[i].uid == uid) {
      existingSlot = i;
      break;
    }
  }

  int slot;
  bool isNew = (existingSlot < 0);

  if (isNew) {
    slot = totalCount.load() % MAX_NOTIFICATIONS;
    if (notifications[slot].valid) {
      Serial.printf(">> Overwriting old notification in slot %d: %s\n",
                    slot, notifications[slot].title);
      if (currentIndex.load() == slot) {
        Serial.println(">> CurrentIndex was pointing to overwritten slot");
      }
    }
  } else {
    slot = existingSlot;
  }

  // CRITICAL: Validate notifications array hasn't been corrupted
  VALIDATE_PTR_RET(notifications, "notifications array before slot access", false);
  
  // CRITICAL: Bounds check
  if (slot < 0 || slot >= MAX_NOTIFICATIONS) {
    Serial.printf("ERROR: Invalid slot %d (max %d)\n", slot, MAX_NOTIFICATIONS);
    return false;
  }

  StoredNotification* n = &notifications[slot];
  
  // CRITICAL: Validate the pointer we just created
  VALIDATE_PTR_RET(n, "notification slot pointer", false);

  setIconFromAppId(appId, categoryId, n->iconId, sizeof(n->iconId));
  Serial.printf(">> Icon set to: %s\n", n->iconId);

  sanitizeString(n->title, title, sizeof(n->title));
  sanitizeString(n->subtitle, subtitle, sizeof(n->subtitle));
  sanitizeString(n->message, message, sizeof(n->message));

  n->dateTime = parseDateTime(dateTime);
  n->uid = uid;
  n->categoryId = categoryId;
  n->important = important;
  n->hasPositiveAction = hasPositiveAction;
  n->hasNegativeAction = hasNegativeAction;
  n->viewed = false;
  n->valid = true;

  if (positiveActionLabel && strlen(positiveActionLabel) > 0) {
    strncpy(n->positiveActionLabel, positiveActionLabel, sizeof(n->positiveActionLabel) - 1);
    n->positiveActionLabel[sizeof(n->positiveActionLabel) - 1] = '\0';
  } else {
    strcpy(n->positiveActionLabel, "Accept");
  }

  if (negativeActionLabel && strlen(negativeActionLabel) > 0) {
    strncpy(n->negativeActionLabel, negativeActionLabel, sizeof(n->negativeActionLabel) - 1);
    n->negativeActionLabel[sizeof(n->negativeActionLabel) - 1] = '\0';
  } else {
    strcpy(n->negativeActionLabel, "Dismiss");
  }

  if (isNew) {
    int newTotal = totalCount.load() + 1;
    totalCount = newTotal;
    newestIndex = slot;
    Serial.printf(">> Added NEW notification to slot %d (total=%d): %s\n",
                  slot, newTotal, n->title);
  } else {
    Serial.printf(">> Updated notification in slot %d: %s\n", slot, n->title);
  }

  int currIdx = currentIndex.load();
  if (currIdx < 0) {
    currentIndex = slot;
  }

  unsigned long now = ::millis();
  Serial.printf(">> Quick notification check: now=%lu, startupTime=%lu, diff=%lu\n",
                now, startupTime, now - startupTime);

  if (now - startupTime < STARTUP_QUIET_PERIOD_MS) {
    Serial.println(">> Skipping quick notification during startup quiet period");
  } else {
    Serial.println(">> Queueing quick notification");
    queueQuickNotification(
      n->iconId,
      n->title,
      n->subtitle,
      n->message,
      n->important);
  }

  return true;
}

void NotificationStore::showNext() {
  if (!initialized.load() || !notifications || totalCount.load() == 0) {
    Serial.println(">> showNext: Not ready or no notifications");
    return;
  }

  int total = totalCount.load();
  int curr = currentIndex.load();

  if (curr < 0 || curr >= MAX_NOTIFICATIONS) {
    Serial.println(">> showNext: Invalid current index");
    return;
  }

  int maxIdx = (total > MAX_NOTIFICATIONS) ? MAX_NOTIFICATIONS : total;

  for (int i = 1; i <= maxIdx; i++) {
    int nextIdx = (curr + i) % MAX_NOTIFICATIONS;
    if (nextIdx < 0 || nextIdx >= MAX_NOTIFICATIONS) continue;

    if (notifications[nextIdx].valid) {
      currentIndex = nextIdx;
      notifications[nextIdx].viewed = true;
      updateDisplay();
      return;
    }
  }
  Serial.println(">> showNext: no valid next found");
}

void NotificationStore::showPrevious() {
  if (!initialized.load() || !notifications || totalCount.load() == 0) {
    Serial.println(">> showPrevious: Not ready or no notifications");
    return;
  }

  int total = totalCount.load();
  int curr = currentIndex.load();

  if (curr < 0 || curr >= MAX_NOTIFICATIONS) {
    Serial.println(">> showPrevious: Invalid current index");
    return;
  }

  int maxIdx = (total > MAX_NOTIFICATIONS) ? MAX_NOTIFICATIONS : total;

  for (int i = 1; i <= maxIdx; i++) {
    int prevIdx = (curr - i + MAX_NOTIFICATIONS) % MAX_NOTIFICATIONS;
    if (prevIdx < 0 || prevIdx >= MAX_NOTIFICATIONS) continue;

    if (notifications[prevIdx].valid) {
      currentIndex = prevIdx;
      notifications[prevIdx].viewed = true;
      updateDisplay();
      return;
    }
  }
  Serial.println(">> showPrevious: no valid prev found");
}

void NotificationStore::showLatest() {
  if (!initialized.load() || !notifications || totalCount.load() == 0) {
    Serial.println(">> showLatest: Not ready or no notifications");
    return;
  }

  int newest = newestIndex.load();

  if (newest < 0 || newest >= MAX_NOTIFICATIONS) {
    Serial.println(">> showLatest: Invalid newest index");
    return;
  }

  if (notifications[newest].valid) {
    currentIndex = newest;
    notifications[newest].viewed = true;
    updateDisplay();
  }
}

int NotificationStore::getUnviewedCount() {
  if (!initialized.load() || !notifications) return 0;

  int count = 0;
  int total = getTotalCount();

  for (int i = 0; i < total && i < MAX_NOTIFICATIONS; i++) {
    if (notifications[i].valid && !notifications[i].viewed) {
      count++;
    }
  }
  return count;
}

int NotificationStore::getCurrentIndex() {
  return currentIndex.load();
}

uint32_t NotificationStore::getCurrentUID() {
  if (!notifications) {
    Serial.println(">> getCurrentUID: notifications is NULL");
    return 0;
  }

  int idx = currentIndex.load();

  if (idx >= 0 && idx < MAX_NOTIFICATIONS && notifications[idx].valid) {
    return notifications[idx].uid;
  }
  return 0;
}

bool NotificationStore::currentHasPositiveAction() {
  if (!notifications) {
    Serial.println(">> currentHasPositiveAction: notifications is NULL");
    return false;
  }

  int idx = currentIndex.load();

  if (idx >= 0 && idx < MAX_NOTIFICATIONS && notifications[idx].valid) {
    return notifications[idx].hasPositiveAction;
  }
  return false;
}

bool NotificationStore::currentHasNegativeAction() {
  if (!notifications) {
    Serial.println(">> currentHasNegativeAction: notifications is NULL");
    return false;
  }

  int idx = currentIndex.load();

  if (idx >= 0 && idx < MAX_NOTIFICATIONS && notifications[idx].valid) {
    return notifications[idx].hasNegativeAction;
  }
  return false;
}

void NotificationStore::updateDisplay() {
  // CRITICAL: Validate object state
  VALIDATE_PTR(this, "NotificationStore object in updateDisplay");
  
  static char mergedTitle[260];
  static char relativeTime[32];

  // CRITICAL FIX #1: Acquire mutex with timeout to protect notifications array access
  if (!notificationsMutex) {
    Serial.println(">> ERROR: notificationsMutex is NULL!");
    return;
  }

  if (xSemaphoreTake(notificationsMutex, pdMS_TO_TICKS(100)) != pdTRUE) {
    Serial.println(">> ERROR: updateDisplay could not acquire mutex (timeout)");
    return;
  }

  // CRITICAL FIX #2: Verify notifications array is valid AFTER acquiring mutex
  if (!notifications) {
    Serial.println(">> FATAL: updateDisplay: notifications is NULL!");
    xSemaphoreGive(notificationsMutex);  // Release mutex before return
    
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_VISIBLE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_PREV, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_NEXT, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_POSITIVE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_NEGATIVE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_COUNT, eez::IntegerValue(0));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_UNVIEWED, eez::IntegerValue(0));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_ICON, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_TITLE, eez::StringValue("Notifications"));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_MESSAGE, eez::StringValue("No notifications"));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_DATETIME, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_IMPORTANT, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_POSITIVE_LABEL, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_NEGATIVE_LABEL, eez::StringValue(""));
    return;
  }

  int idx = currentIndex.load();
  int total = getTotalCount();

  // Debug output removed to prevent Serial buffer overflow

  // CRITICAL FIX #3: Double-check bounds and validity before dereferencing
  if (idx < 0 || idx >= MAX_NOTIFICATIONS || total == 0) {
    xSemaphoreGive(notificationsMutex);  // Release mutex before return
    
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_VISIBLE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_PREV, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_NEXT, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_POSITIVE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_NEGATIVE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_COUNT, eez::IntegerValue(0));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_UNVIEWED, eez::IntegerValue(0));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_ICON, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_TITLE, eez::StringValue("Notifications"));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_MESSAGE, eez::StringValue("No notifications"));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_DATETIME, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_IMPORTANT, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_POSITIVE_LABEL, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_NEGATIVE_LABEL, eez::StringValue(""));
    return;
  }

  // CRITICAL FIX #4: Verify notifications[idx] is valid before dereferencing
  if (!notifications[idx].valid) {
    xSemaphoreGive(notificationsMutex);  // Release mutex before return
    
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_VISIBLE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_PREV, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_NEXT, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_POSITIVE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_NEGATIVE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_COUNT, eez::IntegerValue(0));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_UNVIEWED, eez::IntegerValue(0));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_ICON, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_TITLE, eez::StringValue("Notifications"));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_MESSAGE, eez::StringValue("No notifications"));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_DATETIME, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_IMPORTANT, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_POSITIVE_LABEL, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_NEGATIVE_LABEL, eez::StringValue(""));
    return;
  }

  StoredNotification* n = &notifications[idx];
  
  // CRITICAL: Validate the notification pointer itself
  VALIDATE_PTR(n, "notification pointer in updateDisplay");
  
  // CRITICAL: Validate string data before passing to EEZ Flow
  // Check for corruption by ensuring strings are null-terminated and printable
  auto validateString = [](const char* str, size_t maxLen) -> bool {
    if (!str) return false;
    if ((void*)str == (void*)0xFFFFFFFF) return false;
    
    // Check if string is null-terminated within bounds
    size_t len = 0;
    for (size_t i = 0; i < maxLen; i++) {
      if (str[i] == '\0') {
        return true;  // Found null terminator, string is valid
      }
      len++;
    }
    return false;  // No null terminator found - corrupted!
  };
  
  // Validate all string fields
  if (!validateString(n->iconId, sizeof(n->iconId)) ||
      !validateString(n->title, sizeof(n->title)) ||
      !validateString(n->subtitle, sizeof(n->subtitle)) ||
      !validateString(n->message, sizeof(n->message)) ||
      !validateString(n->positiveActionLabel, sizeof(n->positiveActionLabel)) ||
      !validateString(n->negativeActionLabel, sizeof(n->negativeActionLabel))) {
    
    Serial.printf(">> ERROR: Notification %d has corrupted string data, clearing EEZ variables\n", idx);
    xSemaphoreGive(notificationsMutex);
    
    // Clear to safe defaults
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_VISIBLE, eez::BooleanValue(false));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_ICON, eez::StringValue(""));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_TITLE, eez::StringValue("Error"));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_MESSAGE, eez::StringValue("Corrupted data"));
    return;
  }

  n->viewed = true;

  int unviewed = getUnviewedCount();
  int oldest = findOldestValidIndex();
  int newest = findNewestValidIndex();

  bool hasPrev = (total > 1) && (idx != oldest);
  bool hasNext = (total > 1) && (idx != newest);

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_COUNT, eez::IntegerValue(total));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_UNVIEWED, eez::IntegerValue(unviewed));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_ICON, eez::StringValue(n->iconId));

  bool hasTitle = (n->title[0] != '\0');
  bool hasSubtitle = (n->subtitle[0] != '\0');

  if (hasTitle && hasSubtitle) {
    snprintf(mergedTitle, sizeof(mergedTitle), "%s - %s", n->title, n->subtitle);
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_TITLE, eez::StringValue(mergedTitle));
  } else if (hasTitle) {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_TITLE, eez::StringValue(n->title));
  } else if (hasSubtitle) {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_TITLE, eez::StringValue(n->subtitle));
  } else {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_TITLE, eez::StringValue(""));
  }

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_MESSAGE, eez::StringValue(n->message));

  formatRelativeTime(n->dateTime, relativeTime, sizeof(relativeTime));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_DATETIME, eez::StringValue(relativeTime));

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_VISIBLE, eez::BooleanValue(true));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_PREV, eez::BooleanValue(hasPrev));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_NEXT, eez::BooleanValue(hasNext));

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_IMPORTANT, eez::BooleanValue(n->important));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_POSITIVE, eez::BooleanValue(n->hasPositiveAction));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_HAS_NEGATIVE, eez::BooleanValue(n->hasNegativeAction));

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_POSITIVE_LABEL, eez::StringValue(n->positiveActionLabel));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_NOTIFICATION_NEGATIVE_LABEL, eez::StringValue(n->negativeActionLabel));

  Serial.printf(">> Display updated: [%d/%d] %s (unviewed=%d)\n",
                idx + 1, total, n->title, unviewed);

  // CRITICAL FIX #5: ALWAYS release mutex before returning
  xSemaphoreGive(notificationsMutex);
}

time_t NotificationStore::parseDateTime(const char* dateTimeString) {
  struct tm t;
  int year, month, day, hour, min, sec;

  if (sscanf(dateTimeString, "%4d%2d%2dT%2d%2d%2d", &year, &month, &day, &hour, &min, &sec) == 6) {
    t.tm_year = year - 1900;
    t.tm_mon = month - 1;
    t.tm_mday = day;
    t.tm_hour = hour;
    t.tm_min = min;
    t.tm_sec = sec;
    t.tm_isdst = -1;
    return mktime(&t);
  } else {
    Serial.println("  [DATE]: Failed to parse date string");
  }
  return 0;
}

void NotificationStore::formatRelativeTime(time_t timestamp, char* buffer, size_t bufferSize) const {
  if (!buffer || bufferSize == 0) return;

  time_t now = time(nullptr);
  int64_t diff = now - timestamp;

  if (diff < 0) {
    strncpy(buffer, "in the future", bufferSize - 1);
    buffer[bufferSize - 1] = '\0';
    return;
  }

  if (diff < 60) {
    snprintf(buffer, bufferSize, "%llds ago", (long long)diff);
    return;
  }

  int64_t minutes = diff / 60;
  if (minutes < 60) {
    snprintf(buffer, bufferSize, "%lldm ago", (long long)minutes);
    return;
  }

  int64_t hours = minutes / 60;
  if (hours < 24) {
    snprintf(buffer, bufferSize, "%lldh ago", (long long)hours);
    return;
  }

  int64_t days = hours / 24;
  snprintf(buffer, bufferSize, "%lldd ago", (long long)days);
}

void NotificationStore::removeNotification(uint32_t uid) {
  if (!initialized.load() || !notifications) return;

  int maxSlots = (totalCount.load() > MAX_NOTIFICATIONS) ? MAX_NOTIFICATIONS : totalCount.load();

  for (int i = 0; i < maxSlots; i++) {
    if (notifications[i].valid && notifications[i].uid == uid) {
      notifications[i].valid = false;

      if (currentIndex.load() == i) {
        int newest = findNewestValidIndex();
        if (newest >= 0) {
          Serial.printf(">> After remove, switching to newest valid: slot %d\n", newest);
          currentIndex = newest;
        } else {
          Serial.println(">> After remove, no valid notifications left");
          currentIndex = -1;
          newestIndex = -1;
        }
      }

      updateDisplay();
      return;
    }
  }
}

int NotificationStore::findOldestValidIndex() {
  if (!initialized.load() || !notifications) return -1;

  int newest = newestIndex.load();
  if (newest < 0) return -1;

  int total = totalCount.load();

  int startPos;
  if (total <= MAX_NOTIFICATIONS) {
    startPos = 0;
  } else {
    startPos = (newest + 1) % MAX_NOTIFICATIONS;
  }

  for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
    // CRITICAL FIX: Re-check notifications on each iteration (paranoid but safe)
    if (!notifications) {
      Serial.println(">> ERROR: notifications became NULL during findOldestValidIndex");
      return -1;
    }
    
    int idx = (startPos + i) % MAX_NOTIFICATIONS;
    if (notifications[idx].valid) {
      return idx;
    }
  }

  return -1;
}

int NotificationStore::findNewestValidIndex() {
  if (!initialized.load() || !notifications) return -1;

  int startPos = newestIndex.load();
  if (startPos < 0) return -1;

  for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
    // CRITICAL FIX: Re-check notifications on each iteration (paranoid but safe)
    if (!notifications) {
      Serial.println(">> ERROR: notifications became NULL during findNewestValidIndex");
      return -1;
    }
    
    int idx = (startPos - i + MAX_NOTIFICATIONS) % MAX_NOTIFICATIONS;
    if (notifications[idx].valid) {
      return idx;
    }
  }

  return -1;
}

int NotificationStore::getTotalCount() {
  if (!initialized.load() || !notifications) return 0;

  int count = 0;
  for (int i = 0; i < MAX_NOTIFICATIONS; i++) {
    // CRITICAL FIX: Re-check notifications on each iteration (paranoid but safe against race conditions)
    if (!notifications) {
      Serial.println(">> ERROR: notifications became NULL during getTotalCount");
      break;
    }
    if (notifications[i].valid) {
      count++;
    }
  }
  return count;
}