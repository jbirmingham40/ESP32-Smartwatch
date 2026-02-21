#include <stdlib.h>
#include "esp_heap_caps.h"
#include <Arduino.h>
#include <notifications.h>

extern NotificationStore notificationStore;


void *psram_calloc(size_t n, size_t size) {
  void *ptr = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!ptr) {
    // Fallback to internal heap for small allocs or if PSRAM fails
    ptr = heap_caps_calloc(n, size, MALLOC_CAP_8BIT);
  }
  return ptr;
}

void psram_free(void *ptr) {
  heap_caps_free(ptr);  // works for both PSRAM and internal pointers
}

void* operator new(size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void* operator new[](size_t size) {
    return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void operator delete(void* ptr) noexcept {
    heap_caps_free(ptr);
}

void operator delete[](void* ptr) noexcept {
    heap_caps_free(ptr);
}

// Global memory pressure state
enum MemoryPressure {
  MEMORY_OK = 0,        // > 50KB free
  MEMORY_LOW = 1,       // 35-50KB free - reduce non-critical operations
  MEMORY_CRITICAL = 2   // < 35KB free - emergency mode
};

static MemoryPressure currentMemoryPressure = MEMORY_OK;

MemoryPressure getMemoryPressure() {
  return currentMemoryPressure;
}

bool isMemoryLow() {
  return currentMemoryPressure >= MEMORY_LOW;
}

void monitor_heap() {
  static unsigned long lastCheck = 0;
  static size_t lastMinHeap = SIZE_MAX;
  static int lowHeapCount = 0;
  static unsigned long lastCriticalWarning = 0;
  unsigned long now = millis();

  size_t freeHeap = ESP.getFreeHeap();
  size_t minFreeHeap = ESP.getMinFreeHeap();

  // Update memory pressure level
  MemoryPressure previousPressure = currentMemoryPressure;
  
  if (freeHeap > 35000) {
    currentMemoryPressure = MEMORY_OK;
  } else if (freeHeap > 20000) {
    currentMemoryPressure = MEMORY_LOW;
  } else {
    currentMemoryPressure = MEMORY_CRITICAL;
  }
  
  // Alert when pressure level changes
  if (currentMemoryPressure != previousPressure) {
    Serial.println(F("========================================"));
    Serial.printf(">> MEMORY PRESSURE CHANGE: %d -> %d\n", previousPressure, currentMemoryPressure);
    Serial.printf(">> Free heap: %d bytes (min: %d)\n", freeHeap, minFreeHeap);
    
    if (currentMemoryPressure == MEMORY_LOW) {
      Serial.println(">> MEMORY LOW - Reducing non-critical operations");
    } else if (currentMemoryPressure == MEMORY_CRITICAL) {
      Serial.println(">> MEMORY CRITICAL - Emergency mode!");
      Serial.println(">> Pausing: Weather updates, notifications, UI animations");
    } else {
      Serial.println(">> Memory pressure relieved - Resuming normal operations");
    }
    Serial.println(F("========================================"));
  }

  // Emergency measures when critically low
  if (currentMemoryPressure == MEMORY_CRITICAL) {
    if (now - lastCriticalWarning > 5000) {  // Warn every 5 seconds
      lastCriticalWarning = now;
      Serial.println(F("!! CRITICAL: Free heap < 35KB - System unstable !!"));
      
      // Force garbage collection
      heap_caps_check_integrity_all(true);
    }
  }

  // Check min heap more frequently to catch drops
  if (minFreeHeap < lastMinHeap - 5000) {
    Serial.printf(">> HEAP DROP DETECTED! min=%d (was %d)\n", minFreeHeap, lastMinHeap);
    lastMinHeap = minFreeHeap;
  }

  if (now - lastCheck < 30000) return;
  lastCheck = now;

  size_t freePsram = ESP.getFreePsram();

  Serial.println(F("======================================"));
  Serial.printf("Free heap: %d bytes (min: %d) [%s]\n", 
                freeHeap, minFreeHeap, 
                currentMemoryPressure == MEMORY_OK ? "OK" : 
                currentMemoryPressure == MEMORY_LOW ? "LOW" : "CRITICAL");
  Serial.printf("Free PSRAM: %d bytes\n", freePsram);
  Serial.printf("Notifications: %d total, %d unviewed\n",
                notificationStore.getTotalCount(),
                notificationStore.getUnviewedCount());
  Serial.println(F("======================================"));

  // Warn if heap is getting low
  if (freeHeap < 35000) {
    Serial.println(">> WARNING: Low heap memory!");
    // Check heap integrity when low
    if (!heap_caps_check_integrity_all(true)) {
      Serial.println(">> CRITICAL: Heap corruption detected!");
    }
  }

  lastMinHeap = minFreeHeap;  // Update tracking
}
