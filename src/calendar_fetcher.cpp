#include "src/eez-flow.h"
#include "calendar_fetcher.h"
#include "src/actions.h"
#include "src/ui.h"
#include "src/vars.h"
#include "src/screens.h"
#include <esp_heap_caps.h> 

CalendarFetcher calendarFetcher;

void action_calendar_date_changed(lv_event_t* e) {
  int monthIndex = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_MONTH).getInt();
  int yearIndex = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_YEAR).getInt();
  int dayIndex = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_DAY).getInt();

  int month = monthIndex + 1;
  int year = yearIndex + 2026;
  int day = dayIndex + 1;

  Serial.println("Date entered: " + String(year) + "-" + String(month) + "-" + String(day));
  calendarFetcher.setDate(year, month, day);
  calendarFetcher.requestFetch();
}

void action_calendar_picker_update_days(lv_event_t* e) {
  int monthIndex = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_MONTH).getInt();
  int yearIndex = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_YEAR).getInt();

  int days = calendarFetcher.getDaysInMonth(monthIndex + 1, yearIndex + 2026);  // ui starts with 2026 and remember these are indicies not values
  const char* twentyEightDays = "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28";
  const char* twentyNineDays = "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29";
  const char* thirtyDays = "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30";
  const char* thirtyOneDays = "01\n02\n03\n04\n05\n06\n07\n08\n09\n10\n11\n12\n13\n14\n15\n16\n17\n18\n19\n20\n21\n22\n23\n24\n25\n26\n27\n28\n29\n30\n31";

  const char* daysStr;

  switch (days) {
    case 28: daysStr = twentyEightDays; break;
    case 29: daysStr = twentyNineDays; break;
    case 30: daysStr = thirtyDays; break;
    case 31: daysStr = thirtyOneDays; break;
  }

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_DAY_OPTIONS, eez::StringValue(daysStr));
}

void action_calendar_prev_day(lv_event_t* e) {
  Serial.println("Previous day button pressed");
  calendarFetcher.prevDay();
  calendarFetcher.requestFetch();
}

void action_calendar_next_day(lv_event_t* e) {
  Serial.println("Next day button pressed");
  calendarFetcher.nextDay();
  calendarFetcher.requestFetch();
}

void action_calendar_today(lv_event_t* e) {
  Serial.println("Today button pressed");
  calendarFetcher.goToToday();
  calendarFetcher.requestFetch();
}

void action_calendar_refresh(lv_event_t* e) {
  Serial.println("Loading calendar screen...");
  calendarFetcher.goToToday();
  calendarFetcher.requestFetch();
}

CalendarFetcher::CalendarFetcher() {
  eventsMutex = xSemaphoreCreateMutex();

  // Initialize to today
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    Serial.println("Failed to obtain time");
  }

  timeinfo.tm_hour = 0;
  timeinfo.tm_min = 0;
  timeinfo.tm_sec = 0;
  currentDate = mktime(&timeinfo);
}

// Check if a year is a leap year
bool CalendarFetcher::isLeapYear(int year) {
  // Divisible by 4
  if (year % 4 != 0) {
    return false;
  }
  // Century years must be divisible by 400
  if (year % 100 == 0) {
    return year % 400 == 0;
  }
  return true;
}

// Get number of days in a month
// month: 1-12 (1 = January, 12 = December)
int CalendarFetcher::getDaysInMonth(int month, int year) {
  // Days in each month (non-leap year)
  int days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };

  // Validate month
  if (month < 1 || month > 12) {
    return -1;  // Invalid month
  }

  // Get base days for the month
  int numDays = days[month - 1];

  // Add extra day for February in leap years
  if (month == 2 && isLeapYear(year)) {
    numDays = 29;
  }

  return numDays;
}

// Update the calendar display with current date and events
void CalendarFetcher::updateCalendarDisplay() {

  // Update the date picker defaults
  updateDatePickerDefaults();

  // Get formatted date
  String dateStr = calendarFetcher.getFormattedDate();

  Serial.println("=== " + dateStr + " ===");

  // TODO: Update your EEZ Studio UI date label here
  // setDateLabel(dateStr);

  // Snapshot the events vector under the mutex so Core 0 can't free/modify
  // it while we're iterating below (the LVGL calls that follow can be slow).
  std::vector<CalendarEvent> snapshot;
  xSemaphoreTake(eventsMutex, portMAX_DELAY);
  snapshot = events;  // deep copy — Strings are fully copied
  xSemaphoreGive(eventsMutex);

  int eventCount = snapshot.size();

  if (eventCount == 0) {
    Serial.println("No events today");
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_NO_EVENTS_SHOWN,
                                 eez::BooleanValue(true));
  } else {
    Serial.println("Events: " + String(eventCount));

    // Get the current time so we can determine if the event is happening now
    time_t now = time(0);

    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_DATE,
                                 eez::StringValue(dateStr.c_str()));

    // Get the parent container where items will be added
    // Container: calendar_events_container
    lv_obj_t* parent_list = objects.calender_events_container;

    // Clear any previous rows before rebuilding
    lv_obj_clean(parent_list);

    // Track the first in-progress or future event for auto-scroll
    lv_obj_t* scroll_target = NULL;

    // Display each event
    for (int i = 0; i < eventCount; i++) {
      CalendarEvent event = snapshot[i];

      // Show time range if end time exists
      String timeDisplay = event.startTime;
      if (event.endTime.length() > 0 && event.endTime != event.startTime) {
        timeDisplay += " - " + event.endTime;
      }

      // Debug: Show timestamp for sorting verification
      struct tm eventTimeBuf;
      struct tm* eventTime = localtime_r(&event.startTimestamp, &eventTimeBuf);
      if (eventTime != NULL) {
        Serial.print("[");
        Serial.print(i);
        Serial.print("] ");
        Serial.printf("%02d:%02d", eventTime->tm_hour, eventTime->tm_min);
        Serial.print(" (timestamp: ");
        Serial.print(event.startTimestamp);
        Serial.print(") - ");
        Serial.print(timeDisplay);
        Serial.print(" - ");
        Serial.println(event.title);
      } else {
        Serial.print("[");
        Serial.print(i);
        Serial.print("] INVALID_TIME (timestamp: ");
        Serial.print(event.startTimestamp);
        Serial.print(") - ");
        Serial.print(timeDisplay);
        Serial.print(" - ");
        Serial.println(event.title);
      }

      // Container: list row
      lv_obj_t* list_row_container = lv_obj_create(parent_list);
      lv_obj_set_pos(list_row_container, 0, 0);
      lv_obj_set_size(list_row_container, 315, LV_SIZE_CONTENT);
      lv_obj_set_style_pad_left(list_row_container, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_top(list_row_container, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_right(list_row_container, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(list_row_container, 0, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(list_row_container, 0, LV_PART_MAIN);
      lv_obj_add_flag(list_row_container, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_layout(list_row_container, LV_LAYOUT_FLEX, LV_PART_MAIN);
      lv_obj_set_style_flex_flow(list_row_container, LV_FLEX_FLOW_ROW, LV_PART_MAIN);
      lv_obj_set_style_flex_track_place(list_row_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_PART_MAIN);
      lv_obj_set_style_flex_cross_place(list_row_container, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_set_style_flex_main_place(list_row_container, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_PART_MAIN);
      lv_obj_set_style_radius(list_row_container, 2, LV_PART_MAIN);
      lv_obj_set_style_border_color(list_row_container, lv_color_hex(0xff2a2a2a), LV_PART_MAIN);
      lv_obj_set_style_border_opa(list_row_container, 102, LV_PART_MAIN);
      lv_obj_set_style_border_width(list_row_container, 1, LV_PART_MAIN);
      lv_obj_set_style_border_side(list_row_container, LV_BORDER_SIDE_BOTTOM, LV_PART_MAIN);

      // Color bar
      lv_obj_t* line_obj = lv_obj_create(list_row_container);
      lv_obj_set_pos(line_obj, 0, 0);
      lv_obj_set_size(line_obj, 4, 32);

      if (event.endTimestamp <= now) {
        lv_obj_set_style_bg_color(line_obj, lv_color_hex(0xff888888), LV_PART_MAIN);
      } else if (event.startTimestamp <= now && event.endTimestamp >= now) {
        lv_obj_set_style_bg_color(line_obj, lv_color_hex(0xffff2222), LV_PART_MAIN);
        if (scroll_target == NULL) scroll_target = list_row_container;
      } else if (event.startTimestamp >= now) {
        lv_obj_set_style_bg_color(line_obj, lv_color_hex(0xff1e88ff), LV_PART_MAIN);
        if (scroll_target == NULL) scroll_target = list_row_container;
      }

      lv_obj_set_style_bg_opa(line_obj, LV_OPA_COVER, LV_PART_MAIN);
      lv_obj_set_style_radius(line_obj, 2, LV_PART_MAIN);
      lv_obj_set_style_border_width(line_obj, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_all(line_obj, 0, LV_PART_MAIN);

      // Container: event name and time
      lv_obj_t* event_name_time_container = lv_obj_create(list_row_container);
      lv_obj_set_pos(event_name_time_container, 0, 0);
      lv_obj_set_size(event_name_time_container, 300, 50);
      lv_obj_set_style_pad_left(event_name_time_container, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_top(event_name_time_container, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_right(event_name_time_container, 0, LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(event_name_time_container, 0, LV_PART_MAIN);
      lv_obj_set_style_bg_opa(event_name_time_container, 0, LV_PART_MAIN);
      lv_obj_set_style_border_width(event_name_time_container, 0, LV_PART_MAIN);
      lv_obj_set_style_radius(event_name_time_container, 0, LV_PART_MAIN);
      lv_obj_add_flag(event_name_time_container, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_layout(event_name_time_container, LV_LAYOUT_FLEX, LV_PART_MAIN);
      lv_obj_set_style_flex_flow(event_name_time_container, LV_FLEX_FLOW_COLUMN, LV_PART_MAIN);
      lv_obj_set_style_flex_main_place(event_name_time_container, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_set_style_flex_cross_place(event_name_time_container, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);
      lv_obj_set_style_flex_track_place(event_name_time_container, LV_FLEX_ALIGN_CENTER, LV_PART_MAIN);


      // Label: event name
      lv_obj_t* event_name_label = lv_label_create(event_name_time_container);
      lv_obj_set_pos(event_name_label, 0, 0);
      lv_obj_set_size(event_name_label, 290, 20);
      lv_label_set_long_mode(event_name_label, LV_LABEL_LONG_DOT);
      lv_obj_set_style_text_font(event_name_label, &lv_font_montserrat_18, LV_PART_MAIN);
      lv_obj_set_style_pad_top(event_name_label, 4, LV_PART_MAIN);
      lv_obj_set_style_text_color(event_name_label, lv_color_hex(0xffffffff), LV_PART_MAIN);
      lv_label_set_text_fmt(event_name_label, "%s", event.title.c_str());

      // Label: event time
      lv_obj_t* event_time_label = lv_label_create(event_name_time_container);
      lv_obj_set_pos(event_time_label, 0, 0);
      lv_obj_set_size(event_time_label, 290, 20);
      lv_obj_set_style_text_color(event_time_label, lv_color_hex(0xffb0b0b0), LV_PART_MAIN);
      lv_obj_set_style_pad_bottom(event_time_label, 4, LV_PART_MAIN);
      lv_obj_set_style_text_opa(event_time_label, 215, LV_PART_MAIN);
      lv_obj_set_style_text_font(event_time_label, &lv_font_montserrat_16, LV_PART_MAIN);
      lv_label_set_text_fmt(event_time_label, "%s", timeDisplay.c_str());
    }

    // Auto-scroll to the first in-progress or upcoming event
    if (scroll_target != NULL) {
      lv_obj_update_layout(parent_list);  // Force layout so positions are calculated
      lv_obj_scroll_to_view(scroll_target, LV_ANIM_ON);
    }

    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_NO_EVENTS_SHOWN,
                                 eez::BooleanValue(false));
  }

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_SHOW_SPINNER,
                               eez::BooleanValue(false));
}

String CalendarFetcher::urlEncode(String str) {
  String encoded = "";
  char c;
  for (int i = 0; i < str.length(); i++) {
    c = str.charAt(i);
    if (c == ' ') {
      encoded += "%20";
    } else if (c == '@') {
      encoded += "%40";
    } else {
      encoded += c;
    }
  }
  return encoded;
}

String CalendarFetcher::decodeICalString(String str) {
  // Remove line breaks and unescape characters
  str.replace("\\n", " ");
  str.replace("\\,", ",");
  str.replace("\\;", ";");
  str.replace("\\\\", "\\");

  // First pass: remove HTML tags
  String noHtml = "";
  bool inTag = false;
  for (int i = 0; i < str.length(); i++) {
    char c = str.charAt(i);
    if (c == '<') {
      inTag = true;
    } else if (c == '>') {
      inTag = false;
    } else if (!inTag) {
      noHtml += c;
    }
  }

  // Second pass: replace URLs with [link] and strip non-ASCII
  String result = "";
  int i = 0;
  while (i < noHtml.length()) {
    // Check for http:// or https://
    bool isLink = false;
    if (i + 7 <= noHtml.length() && noHtml.substring(i, i + 7) == "http://") {
      isLink = true;
    } else if (i + 8 <= noHtml.length() && noHtml.substring(i, i + 8) == "https://") {
      isLink = true;
    }

    if (isLink) {
      // Skip until we hit whitespace or end of string
      while (i < noHtml.length()) {
        char c = noHtml.charAt(i);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
          break;
        }
        i++;
      }
      result += "[link]";
    } else {
      // Only keep ASCII printable characters (32-126) and common whitespace
      char c = noHtml.charAt(i);
      if ((c >= 32 && c <= 126) || c == '\t' || c == '\n') {
        result += c;
      }
      // Non-ASCII characters are silently dropped
      i++;
    }
  }

  // Clean up multiple spaces
  while (result.indexOf("  ") != -1) {
    result.replace("  ", " ");
  }

  result.trim();
  return result;
}

time_t CalendarFetcher::parseICalDateTime(String dtString) {
  // iCal format: YYYYMMDDTHHMMSS or YYYYMMDDTHHMMSSZ (Z = UTC)
  dtString.trim();  // Remove any whitespace/newlines

  // Check if this is UTC time (ends with Z or z)
  bool isUTC = dtString.endsWith("Z") || dtString.endsWith("z");

  // Debug: show what we're parsing
  //Serial.println("Parsing: " + dtString + (isUTC ? " [UTC]" : " [LOCAL]"));

  // Remove the Z suffix if present
  if (isUTC) {
    dtString.remove(dtString.length() - 1);
  }

  // Remove any colons and dashes
  dtString.replace(":", "");
  dtString.replace("-", "");

  // Extract components
  int year = dtString.substring(0, 4).toInt();
  int month = dtString.substring(4, 6).toInt();
  int day = dtString.substring(6, 8).toInt();
  int hour = 0, minute = 0, second = 0;

  if (dtString.length() >= 15) {
    hour = dtString.substring(9, 11).toInt();
    minute = dtString.substring(11, 13).toInt();
    second = dtString.substring(13, 15).toInt();
  }

  struct tm timeinfo = { 0 };
  timeinfo.tm_year = year - 1900;
  timeinfo.tm_mon = month - 1;
  timeinfo.tm_mday = day;
  timeinfo.tm_hour = hour;
  timeinfo.tm_min = minute;
  timeinfo.tm_sec = second;
  timeinfo.tm_isdst = -1;  // Let the system determine DST

  if (isUTC) {
    // mktime() interprets its input as local time, but we have UTC values.
    // Temporarily switch to UTC so mktime produces the correct epoch, then restore.
    // IMPORTANT: We must copy the TZ string because setenv may invalidate the pointer.
    char origTZ[64] = { 0 };
    bool hadTZ = false;
    const char* tz = getenv("TZ");
    if (tz) {
      hadTZ = true;
      strncpy(origTZ, tz, sizeof(origTZ) - 1);
    }

    setenv("TZ", "UTC0", 1);
    tzset();

    timeinfo.tm_isdst = 0;  // No DST in UTC
    time_t result = mktime(&timeinfo);

    // Restore original timezone
    if (hadTZ) {
      setenv("TZ", origTZ, 1);
    } else {
      unsetenv("TZ");
    }
    tzset();

    return result;
  }

  return mktime(&timeinfo);
}

String CalendarFetcher::formatTime(time_t timestamp) {
  struct tm timeinfoBuf;
  struct tm* timeinfo = localtime_r(&timestamp, &timeinfoBuf);

  // Check if localtime failed
  if (timeinfo == NULL) {
    return "Invalid Time";
  }

  char buffer[20];

  int hour = timeinfo->tm_hour;
  String ampm = "AM";

  if (hour == 0) {
    hour = 12;
  } else if (hour == 12) {
    ampm = "PM";
  } else if (hour > 12) {
    hour -= 12;
    ampm = "PM";
  }

  sprintf(buffer, "%d:%02d %s", hour, timeinfo->tm_min, ampm.c_str());
  return String(buffer);
}

bool CalendarFetcher::eventOccursOnDate(time_t eventStart, String rrule, time_t targetDate) {
  // Get just the date parts (ignore time)
  struct tm eventTmBuf, targetTmBuf;
  localtime_r(&eventStart, &eventTmBuf);
  localtime_r(&targetDate, &targetTmBuf);
  struct tm* eventTm = &eventTmBuf;
  struct tm* targetTm = &targetTmBuf;

  // Check if localtime failed
  if (eventTm == NULL || targetTm == NULL) {
    Serial.println("ERROR: localtime failed in eventOccursOnDate");
    return false;
  }

  // Create date-only timestamps (midnight)
  struct tm eventDateOnly = *eventTm;
  eventDateOnly.tm_hour = 0;
  eventDateOnly.tm_min = 0;
  eventDateOnly.tm_sec = 0;
  time_t eventDateStart = mktime(&eventDateOnly);

  struct tm targetDateOnly = *targetTm;
  targetDateOnly.tm_hour = 0;
  targetDateOnly.tm_min = 0;
  targetDateOnly.tm_sec = 0;
  time_t targetDateStart = mktime(&targetDateOnly);

  // If no RRULE, just check if dates match
  if (rrule.length() == 0) {
    return eventDateStart == targetDateStart;
  }

  // Check if target date is before event starts
  if (targetDateStart < eventDateStart) {
    return false;
  }

  // Parse RRULE for UNTIL date
  time_t untilDate = 0;
  int untilPos = rrule.indexOf("UNTIL=");
  if (untilPos != -1) {
    int untilEnd = rrule.indexOf(";", untilPos);
    if (untilEnd == -1) untilEnd = rrule.length();
    String untilStr = rrule.substring(untilPos + 6, untilEnd);
    untilDate = parseICalDateTime(untilStr);

    // Check if target date is after UNTIL date
    if (targetDateStart > untilDate) {
      return false;
    }
  }

  // Check frequency
  if (rrule.indexOf("FREQ=DAILY") != -1) {
    // Daily recurring event - occurs every day within range
    return true;
  } else if (rrule.indexOf("FREQ=WEEKLY") != -1) {
    // Weekly recurring - check if same day of week
    return eventTm->tm_wday == targetTm->tm_wday;
  }

  // Default: event doesn't recur on this date
  return false;
}

void CalendarFetcher::parseVEvent(String eventData) {
  CalendarEvent event;
  bool allDay = false;
  String rrule = "";

  // Find SUMMARY (title)
  int summaryStart = eventData.indexOf("SUMMARY:");
  if (summaryStart != -1) {
    summaryStart += 8;
    int summaryEnd = eventData.indexOf("\n", summaryStart);
    event.title = eventData.substring(summaryStart, summaryEnd);
    event.title = decodeICalString(event.title);
  }

  // Find DTSTART (start time)
  int dtStartPos = eventData.indexOf("DTSTART");
  if (dtStartPos != -1) {
    int valueStart = eventData.indexOf(":", dtStartPos) + 1;
    int valueEnd = eventData.indexOf("\n", valueStart);
    String dtStart = eventData.substring(valueStart, valueEnd);
    dtStart.trim();

    // Check if it's an all-day event (no time component)
    if (dtStart.length() == 8) {
      allDay = true;
    }

    event.startTimestamp = parseICalDateTime(dtStart);
    event.startTime = allDay ? "All Day" : formatTime(event.startTimestamp);
  }

  // Find DTEND (end time)
  int dtEndPos = eventData.indexOf("DTEND");
  if (dtEndPos != -1) {
    int valueStart = eventData.indexOf(":", dtEndPos) + 1;
    int valueEnd = eventData.indexOf("\n", valueStart);
    String dtEnd = eventData.substring(valueStart, valueEnd);
    dtEnd.trim();

    event.endTimestamp = parseICalDateTime(dtEnd);
    event.endTime = allDay ? "" : formatTime(event.endTimestamp);
  }

  // Find RRULE (recurrence rule)
  int rrulePos = eventData.indexOf("RRULE:");
  if (rrulePos != -1) {
    int rruleStart = rrulePos + 6;
    int rruleEnd = eventData.indexOf("\n", rruleStart);
    rrule = eventData.substring(rruleStart, rruleEnd);
    rrule.trim();
  }

  // Only add if we have a title AND the event occurs on the current viewing date
  if (event.title.length() > 0 && eventOccursOnDate(event.startTimestamp, rrule, currentDate)) {
    // Adjust timestamps to current viewing date (preserving time-of-day) for proper sorting and color logic
    // We must rebase BOTH start and end, otherwise endTimestamp stays on the original occurrence date
    // and the color comparison (endTimestamp <= now) incorrectly marks future recurring events as past.
    time_t duration = event.endTimestamp - event.startTimestamp;

    struct tm eventTmBuf;
    localtime_r(&event.startTimestamp, &eventTmBuf);
    struct tm currentDateBuf;
    localtime_r(&currentDate, &currentDateBuf);

    // Combine current viewing date with event's time-of-day
    currentDateBuf.tm_hour = eventTmBuf.tm_hour;
    currentDateBuf.tm_min = eventTmBuf.tm_min;
    currentDateBuf.tm_sec = eventTmBuf.tm_sec;
    event.startTimestamp = mktime(&currentDateBuf);
    event.endTimestamp = event.startTimestamp + duration;

    // Check for duplicates (same title and overlapping time)
    bool isDuplicate = false;
    for (int i = 0; i < events.size(); i++) {
      if (events[i].title == event.title) {
        // Check if times overlap or are very close (within same hour)
        struct tm existingTmBuf, newTmBuf;
        localtime_r(&events[i].startTimestamp, &existingTmBuf);
        localtime_r(&event.startTimestamp, &newTmBuf);
        struct tm* existingTm = &existingTmBuf;
        struct tm* newTm = &newTmBuf;

        if (existingTm != NULL && newTm != NULL && existingTm->tm_hour == newTm->tm_hour) {
          isDuplicate = true;
          break;
        }
      }
    }

    if (!isDuplicate) {
      events.push_back(event);
    }
  }
}

bool CalendarFetcher::fetchCalendar(String url) {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi not connected");
    return false;
  }

  // Format dates for URL
  time_t startDate = currentDate;
  struct tm startDateBuf;
  struct tm* startDateStruct = localtime_r(&startDate, &startDateBuf);
  if (startDateStruct == NULL) {
    Serial.println("ERROR: localtime failed for startDate");
    return false;
  }
  char startDateStr[11];
  sprintf(startDateStr, "%04d-%02d-%02d",
          startDateStruct->tm_year + 1900,
          startDateStruct->tm_mon + 1,
          startDateStruct->tm_mday);

  // End date is same day (we only fetch one day at a time)
  time_t endDate = currentDate + 86400;
  struct tm endDateBuf;
  struct tm* endDateStruct = localtime_r(&endDate, &endDateBuf);  // Add one day in seconds
  if (endDateStruct == NULL) {
    Serial.println("ERROR: localtime failed for endDate");
    return false;
  }
  char endDateStr[11];
  sprintf(endDateStr, "%04d-%02d-%02d",
          endDateStruct->tm_year + 1900,
          endDateStruct->tm_mon + 1,
          endDateStruct->tm_mday);

  url += "?start-min=" + String(startDateStr);
  url += "&start-max=" + String(endDateStr);

  Serial.println("Fetching: " + url);

  HTTPClient http;
  http.begin(url);

  // OPTIONAL: Uncomment to skip SSL certificate verification (faster but less secure)
  // http.setInsecure();

  // Set timeouts to prevent hanging
  http.setTimeout(10000);        // 10 second timeout for connection
  http.setConnectTimeout(5000);  // 5 second connection timeout

  // Reduce connection reuse issues
  http.setReuse(false);

  int httpCode = http.GET();

  if (httpCode == HTTP_CODE_OK) {
    // OPTIMIZATION: Read HTTP response directly into PSRAM to avoid large internal heap allocation
    // http.getString() would allocate the entire iCal response (often 10-30KB+) on internal heap
    WiFiClient* stream = http.getStreamPtr();
    int contentLength = http.getSize();

    size_t bufSize;
    if (contentLength > 0) {
      bufSize = contentLength + 1;
    } else {
      bufSize = 65536;  // Max expected calendar size for chunked transfer
    }

    char* payload = (char*)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!payload) {
      Serial.println("Failed to allocate PSRAM buffer for calendar response");
      http.end();
      return false;
    }

    size_t totalRead = 0;
    unsigned long readStart = millis();
    while (http.connected() && totalRead < bufSize - 1) {
      size_t avail = stream->available();
      if (avail) {
        size_t toRead = (avail < (bufSize - 1 - totalRead)) ? avail : (bufSize - 1 - totalRead);
        int c = stream->readBytes(payload + totalRead, toRead);
        totalRead += c;
        if (contentLength > 0 && (int)totalRead >= contentLength) break;
      } else {
        // Timeout after 10 seconds of no data
        if (millis() - readStart > 10000) {
          Serial.println("Calendar read timeout");
          break;
        }
        delay(1);
      }
    }
    payload[totalRead] = '\0';

    Serial.printf("Read %d bytes of calendar data to PSRAM\n", totalRead);

    // Parse iCal data using C-string functions to avoid copying back to internal heap
    char* searchPos = payload;
    while (true) {
      char* eventStart = strstr(searchPos, "BEGIN:VEVENT");
      if (!eventStart) break;

      char* eventEnd = strstr(eventStart, "END:VEVENT");
      if (eventEnd) {
        // Create a String only for this individual event (much smaller than full payload)
        size_t eventLen = eventEnd - eventStart;
        String eventData;
        eventData.reserve(eventLen + 1);
        for (size_t i = 0; i < eventLen; i++) {
          eventData += eventStart[i];
        }
        parseVEvent(eventData);
        searchPos = eventEnd + 10;  // Skip past "END:VEVENT"
      } else {
        break;
      }
    }

    heap_caps_free(payload);
    http.end();
    Serial.println("Fetched " + String(events.size()) + " events so far");
    return true;
  } else {
    String errorMsg = "HTTP error: " + String(httpCode);
    if (httpCode == -1) {
      errorMsg += " (Connection timeout - check WiFi signal)";
    } else if (httpCode == -11) {
      errorMsg += " (Connection failed)";
    }
    Serial.println(errorMsg);
    http.end();
    return false;
  }
}

bool CalendarFetcher::fetchCalendarWithRetry(int maxRetries) {

  // Hold the mutex for the entire fetch: clear → populate → sort.
  // This prevents Core 1 from reading a partially-built or freed events vector.
  xSemaphoreTake(eventsMutex, portMAX_DELAY);

  // Clear all events once before fetching any URLs
  clearEvents();

  bool anySuccess = false;

  for(int i=0; i<icalUrls.size(); i++) {
    String url = icalUrls[i];
    bool urlSuccess = false;

    for (int attempt = 0; attempt <= maxRetries; attempt++) {
      if (attempt > 0) {
        Serial.println("Retry attempt " + String(attempt) + " of " + String(maxRetries) + " for calendar " + String(i + 1));
        delay(1000 * attempt);
      }

      if (fetchCalendar(url)) {
        urlSuccess = true;
        anySuccess = true;
        break;  // Success — move on to next URL
      }

      if (attempt < maxRetries) {
        Serial.println("Retrying in " + String(1000 * (attempt + 1)) + "ms...");
      }
    }

    if (!urlSuccess) {
      Serial.println("Failed to fetch calendar " + String(i + 1) + " after " + String(maxRetries + 1) + " attempts, continuing...");
    }
  }

  // Sort merged events from all calendars by start time
  if (anySuccess) {
    std::sort(events.begin(), events.end(),
              [](const CalendarEvent& a, const CalendarEvent& b) {
                return a.startTimestamp < b.startTimestamp;
              });
    Serial.println("Total merged events: " + String(events.size()));
  }

  xSemaphoreGive(eventsMutex);
  return anySuccess;
}

// --- Async fetch support ---
// Static task function — runs fetchCalendarWithRetry on its own stack, then self-deletes.
void CalendarFetcher::fetchTask(void* param) {
  CalendarFetcher* self = static_cast<CalendarFetcher*>(param);

  Serial.println("Calendar fetch task started");
  self->fetchCalendarWithRetry(2);

  // Signal UI to update regardless of success/failure (hides spinner either way)
  self->displayUpdateNeeded = true;
  self->fetchTaskHandle = NULL;  // Clear before delete so requestFetch knows we're done
  Serial.println("Calendar fetch task finished");
  
  // FIX: Yield before self-delete to ensure cleanup completes (prevents stack canary corruption)
  vTaskDelay(pdMS_TO_TICKS(50));
  
  vTaskDelete(xTaskGetCurrentTaskHandle());
}

// Called from UI action handlers (Core 1). Shows spinner and spawns a one-shot fetch task.
void CalendarFetcher::requestFetch() {
  // If a fetch is already running, let it finish — don't stack tasks
  if (fetchTaskHandle != NULL) {
    Serial.println("Calendar fetch already in progress, ignoring request");
    return;
  }

  displayUpdateNeeded = false;

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_SHOW_SPINNER,
                               eez::BooleanValue(true));

  // Spawn one-shot task on Core 0 with 32KB stack in PSRAM for TLS/HTTP
  // FIX: 16KB was insufficient for mbedTLS handshake over HTTPS (Google Calendar
  // uses TLS). Stack overflow corrupted adjacent memory → LoadProhibited crash
  // with EXCVADDR 0x00000043 and CORRUPTED backtrace. 32KB matches artwork task.
  xTaskCreatePinnedToCore(
    fetchTask,
    "CalFetch",
    32768,  // 32KB — mbedTLS TLS handshake needs ~20-24KB stack
    this,   // Pass 'this' so the static function can access the instance
    2,      // Priority below Periodic_Tasks (3) so BLE/time aren't starved
    &fetchTaskHandle,
    0      // Core 0
  );
}

// Called from UI_Loop_Task (Core 1) every loop iteration.
// Returns true if display was updated.
bool CalendarFetcher::checkDisplayUpdate() {
  if (!displayUpdateNeeded) return false;
  displayUpdateNeeded = false;
  updateCalendarDisplay();
  return true;
}

void CalendarFetcher::nextDay() {
  currentDate += 86400;  // Add one day in seconds
}

void CalendarFetcher::prevDay() {
  currentDate -= 86400;  // Subtract one day in seconds
}

void CalendarFetcher::goToToday() {
  struct tm timeinfo;
  if (getLocalTime(&timeinfo)) {
    timeinfo.tm_hour = 0;
    timeinfo.tm_min = 0;
    timeinfo.tm_sec = 0;
    currentDate = mktime(&timeinfo);
  }
}

void CalendarFetcher::setDate(int year, int month, int day) {
  struct tm timeinfo = { 0 };
  timeinfo.tm_year = year - 1900;
  timeinfo.tm_mon = month - 1;
  timeinfo.tm_mday = day;
  timeinfo.tm_hour = 0;
  timeinfo.tm_min = 0;
  timeinfo.tm_sec = 0;
  currentDate = mktime(&timeinfo);
}

void CalendarFetcher::updateDatePickerDefaults() {
  struct tm dateInfoBuf;
  struct tm* dateInfo = localtime_r(&currentDate, &dateInfoBuf);

  if (dateInfo == NULL) {
    Serial.println("ERROR: localtime failed in updateDatePickerDefaults");
    return;
  }

  int yearIndex = 2026 - (dateInfo->tm_year + 1900);  // we start the UI at 2026 and the tm_year is offset by 1900
  int monthIndex = dateInfo->tm_mon;
  int dayIndex = dateInfo->tm_mday - 1;

  Serial.printf("yearIndex %d, dateInfo->tm_year %d\n", yearIndex, dateInfo->tm_year);

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_YEAR, eez::IntegerValue(yearIndex));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_MONTH, eez::IntegerValue(monthIndex));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_CALENDAR_PICKER_DAY, eez::IntegerValue(dayIndex));
}

String CalendarFetcher::getFormattedDate() {
  struct tm dateInfoBuf;
  struct tm* dateInfo = localtime_r(&currentDate, &dateInfoBuf);

  if (dateInfo == NULL) {
    Serial.println("ERROR: localtime failed in getFormattedDate");
    return "Invalid Date";
  }

  const char* days[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
  const char* months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                           "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };

  char buffer[20];
  sprintf(buffer, "%s, %s %d",
          days[dateInfo->tm_wday],
          months[dateInfo->tm_mon],
          dateInfo->tm_mday);

  return String(buffer);
}

int CalendarFetcher::getEventCount() {
  return events.size();
}

CalendarEvent CalendarFetcher::getEvent(int index) {
  if (index >= 0 && index < events.size()) {
    return events[index];
  }
  return CalendarEvent();
}

void CalendarFetcher::clearEvents() {
  // Caller must already hold eventsMutex before calling this.
  events.clear();
}

void CalendarFetcher::serializeConfig(JsonDocument &doc) {
  JsonArray icalArr = doc["ical_urls"].to<JsonArray>();
  for (int i=0; i<icalUrls.size(); i++) {
    icalArr.add(icalUrls[i]);
  }
}

void CalendarFetcher::deserializeConfig(JsonDocument &doc) {
  for (String icalUrl : doc["ical_urls"].as<JsonArray>()) {
    icalUrls.push_back(icalUrl);
  }
}