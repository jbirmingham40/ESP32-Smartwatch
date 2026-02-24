#ifndef CALENDAR_FETCHER_H
#define CALENDAR_FETCHER_H

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <time.h>
#include <vector>
#include <algorithm>
#include <serializable_config.h>

struct CalendarEvent {
  char title[128];
  char startTime[32];
  char endTime[32];
  time_t startTimestamp;
  time_t endTimestamp;
};

class CalendarFetcher : public SerializableConfig {

public:
  CalendarFetcher();

  // Fetch calendar data with retry logic (blocking — used internally by the one-shot task)
  bool fetchCalendarWithRetry(int maxRetries = 2);

  // Async fetch support — call requestFetch() from UI actions (spawns one-shot task),
  // or call fetchCalendarWithRetry() + setDisplayUpdateNeeded() directly from a task
  // that already has sufficient stack (e.g. Background_Tasks with PSRAM stack).
  // checkDisplayUpdate() is called from the UI loop (Core 1).
  void requestFetch();
  void setDisplayUpdateNeeded() { displayUpdateNeeded = true; }
  bool checkDisplayUpdate();
  bool isFetchInProgress() const { return fetchTaskHandle != NULL; }

  // Date navigation
  void nextDay();
  void prevDay();
  void goToToday();
  void setDate(int year, int month, int day);

  // Get calendar data
  String getFormattedDate();
  int getEventCount();
  CalendarEvent getEvent(int index);
  int getDaysInMonth(int month, int year);

  // Update display with current events
  void updateCalendarDisplay();

public:
  void serializeConfig(JsonDocument &doc) override;
  void deserializeConfig(JsonDocument &doc) override;

private:
  time_t currentDate;
  std::vector<CalendarEvent> events;
  std::vector<String> icalUrls;

  // One-shot fetch task
  volatile bool displayUpdateNeeded = false;
  TaskHandle_t fetchTaskHandle = NULL;
  static void fetchTask(void* param);

  // Protects the events vector against concurrent access between
  // Core 0 (fetch task clearing/populating events) and
  // Core 1 (UI loop reading events in updateCalendarDisplay).
  SemaphoreHandle_t eventsMutex = NULL;

  // Fetching and parsing
  bool isLeapYear(int year);
  bool fetchCalendar(String url);
  void parseVEvent(String eventData);
  time_t parseICalDateTime(String dtString);
  String formatTime(time_t timestamp);
  String decodeICalString(String str);
  String urlEncode(String str);
  void updateDatePickerDefaults();
  bool eventOccursOnDate(time_t eventStart, String rrule, time_t targetDate);
  void clearEvents();
};

#endif
