#include <ctime>
#include "Audio_PCM5101.h"
#include <unistd.h>
#include "src/eez-flow.h"
#include "Arduino.h"
#include "src/ui.h"
#include "src/vars.h"
#include "time.h"
#include "BAT_Driver.h"
#include "src/actions.h"
#include "time_client.h"
#include "src/screens.h"
#include "alarm_timer.h"
#include "serializable_configs.h"

extern SerializableConfigs serializableConfigs;

// CRITICAL FIX: Debouncing to prevent rapid alarm triggering
static unsigned long lastAlarmCheck = 0;
static unsigned long lastAlarmTrigger = 0;
static unsigned long lastTimerTrigger = 0;
static bool alarmCurrentlyPlaying = false;
static bool timerCurrentlyPlaying = false;

const unsigned long ALARM_CHECK_DEBOUNCE_MS = 1000;   // Only check once per second
const unsigned long ALARM_RETRIGGER_DELAY_MS = 5000;  // 5 seconds between alarm triggers
const unsigned long TIMER_RETRIGGER_DELAY_MS = 5000;  // 5 seconds between timer triggers

void playSound(const char *directory, const char *filename) {
  Serial.println(F("Playing alarm"));
  Play_Music(directory, filename);
}

void playSound(int sound) {
  switch (sound) {
    case 0: break;  // no sound
    case 1: playSound("/System/alarms", "alarm-clock-beep-988-2.wav"); break;
    case 2: playSound("/System/alarms", "alarm-digital-clock-beep-989-2.wav"); break;
    case 3: playSound("/System/alarms", "alarm-tone-996.wav"); break;
    case 4: playSound("/System/alarms", "alert-alarm-1005.wav"); break;
    case 5: playSound("/System/alarms", "casino-jackpot-alarm-and-coins-1991.wav"); break;
    case 6: playSound("/System/alarms", "casino-win-alarm-and-coins-1990.wav"); break;
    case 7: playSound("/System/alarms", "city-alert-siren-loop-1008.wav"); break;
    case 8: playSound("/System/alarms", "classic-alarm-995.wav"); break;
    case 9: playSound("/System/alarms", "classic-short-alarm-993-2.wav"); break;
    case 10: playSound("/System/alarms", "critical-alarm-1004.wav"); break;
    case 11: playSound("/System/alarms", "data-scanner-2847.wav"); break;
    case 12: playSound("/System/alarms", "digital-clock-digital-alarm-buzzer-992-2.wav"); break;
    case 13: playSound("/System/alarms", "facility-alarm-908-2.wav"); break;
    case 14: playSound("/System/alarms", "facility-alarm-sound-999.wav"); break;
    case 15: playSound("/System/alarms", "game-notification-wave-alarm-987-2.wav"); break;
    case 16: playSound("/System/alarms", "interface-hint-notification-911-2.wav"); break;
    case 17: playSound("/System/alarms", "morning-clock-alarm-1003.wav"); break;
    case 18: playSound("/System/alarms", "retro-game-emergency-alarm-1000.wav"); break;
    case 19: playSound("/System/alarms", "rooster-crowing-in-the-morning-2462-2.wav"); break;
    case 20: playSound("/System/alarms", "security-facility-breach-alarm-994.wav"); break;
    case 21: playSound("/System/alarms", "short-rooster-crowing-2470-2.wav"); break;
    case 22: playSound("/System/alarms", "slot-machine-win-alarm-1995.wav"); break;
    case 23: playSound("/System/alarms", "sound-alert-in-hall-1006.wav"); break;
    case 24: playSound("/System/alarms", "space-shooter-alarm-1002.wav"); break;
    case 25: playSound("/System/alarms", "spaceship-alarm-998.wav"); break;
    case 26: playSound("/System/alarms", "street-public-alarm-997.wav"); break;
    case 27: playSound("/System/alarms", "vintage-warning-alarm-990-2.wav"); break;
  }
}

void action_check_alarm_native(lv_event_t *e) {
  unsigned long now = millis();

  if (now - lastAlarmCheck < ALARM_CHECK_DEBOUNCE_MS) {
    return;
  }
  lastAlarmCheck = now;

  if (alarmCurrentlyPlaying) {
    return;
  }

  // Don't trigger again if we recently triggered
  if (now - lastAlarmTrigger < ALARM_RETRIGGER_DELAY_MS) {
    return;
  }

  time_t alarmNextAlarm = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_NEXT_ALARM).getUInt32();
  time_t nowLong;
  time(&nowLong);

  if (nowLong >= alarmNextAlarm) {
    Serial.println(F(">> ALARM TRIGGERED"));
    // DON'T set lastAlarmTrigger here - let play_alarm_sound set it
    alarmCurrentlyPlaying = true;
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_TRIGGERED, eez::BooleanValue(true));
  }
}

void action_play_alarm_sound(lv_event_t *e) {
  Serial.println(F("Play Alarm"));

  // Set the trigger time HERE when sound actually plays
  lastAlarmTrigger = millis();

  int soundIndex = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_SOUND).getInt();

  Serial.printf("Playing sound %d\n", soundIndex);
  playSound(soundIndex);  // This blocks for a few seconds
}

void action_update_next_alarm_string(lv_event_t *e) {
  time_t nowLong = time(0);
  struct tm *now = localtime(&nowLong);

  int alarmHour = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_HOUR).getInt32();
  bool alarmAmPm = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_AM_PM).getBoolean();
  int alarmMinute = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_MINUTE).getInt32();

  // Convert from 0-11 storage to 1-12 UI value, then to 24-hour
  int uiHour = alarmHour + 1;  // 0-11 → 1-12

  if (alarmAmPm) {
    // PM
    alarmHour = (uiHour == 12) ? 12 : (uiHour + 12);  // 12 PM→12, 1-11 PM→13-23
  } else {
    // AM
    alarmHour = (uiHour == 12) ? 0 : uiHour;  // 12 AM→0, 1-11 AM→1-11
  }

  // Build alarm time for today
  struct tm alarmTime = *now;
  alarmTime.tm_hour = alarmHour;
  alarmTime.tm_min = alarmMinute;
  alarmTime.tm_sec = 0;

  time_t alarmNextAlarm = mktime(&alarmTime);

  // If alarm has passed, add 24 hours
  if (alarmNextAlarm <= nowLong) {
    alarmNextAlarm += 86400;
  }

  // Calculate time until alarm
  long secondsTillAlarm = alarmNextAlarm - nowLong;
  int totalMinutes = (secondsTillAlarm + 59) / 60;
  int hoursTillAlarm = totalMinutes / 60;
  int minutesTillAlarm = totalMinutes % 60;

  bool isToday = (secondsTillAlarm < 86400);

  static char nextAlarmString[17] = { 0 };
  snprintf(nextAlarmString, sizeof(nextAlarmString), "%s, %dh %dm",
           isToday ? "Today" : "Tomorrow", hoursTillAlarm, minutesTillAlarm);

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_NEXT_ALARM_STRING, eez::StringValue(nextAlarmString));
}

void action_snooze_alarm(lv_event_t *e) {
  time_t nowLong = time(0);
  struct tm *now = localtime(&nowLong);

  int snoozeOption = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_SNOOZE).getInt32();
  int snoozeMinutes;
  const char *snoozeString;

  switch (snoozeOption) {
    case 0:
      snoozeMinutes = 0;
      snoozeString = "1 minute";
      break;
    case 1:
      snoozeMinutes = 5;
      snoozeString = "5 minutes";
      break;
    case 2:
      snoozeMinutes = 10;
      snoozeString = "10 minutes";
      break;
    case 3:
      snoozeMinutes = 15;
      snoozeString = "15 minutes";
      break;
    case 4:
      snoozeMinutes = 20;
      snoozeString = "20 minutes";
      break;
    case 5:
      snoozeMinutes = 30;
      snoozeString = "30 minutes";
      break;
  }

  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_SNOOZE_TIME_PERIOD, eez::StringValue(snoozeString));

  // add one minute to the snooze minutes since we are triggering at the start of the minute and may end up triggering less than a minute
  // better to be a some seconds late than early
  snoozeMinutes += 1;

  // get current value, increment, and update
  time_t alarmNextAlarm = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_NEXT_ALARM).getInt32();
  alarmNextAlarm += snoozeMinutes * 60;

  struct tm *alarmTime = localtime(&alarmNextAlarm);
  
  // remember the values are indices into the roller which for hour starts at 01 not 00 and am/pm is 0 and 1
  // for hour we need to subtract one
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_HOUR, eez::IntegerValue(alarmTime->tm_hour == 0 ? 23 : alarmTime->tm_hour - 1));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_AM_PM, eez::IntegerValue(alarmTime->tm_hour < 12 ? 0 : 1));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_MINUTE, eez::IntegerValue(alarmTime->tm_min));

  Serial.printf("Setting next alarm to: %ld\n", alarmNextAlarm);
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_NEXT_ALARM, eez::IntegerValue(alarmNextAlarm));
}

void action_activate_alarm(lv_event_t *e) {
  time_t nowLong = time(0);
  struct tm *now = localtime(&nowLong);

  int alarmHour = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_HOUR).getInt32();
  bool alarmAmPm = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_AM_PM).getBoolean();
  int alarmMinute = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_MINUTE).getInt32();

  // Convert from 0-11 storage to 1-12 UI value, then to 24-hour
  int uiHour = alarmHour + 1;  // 0-11 → 1-12

  if (alarmAmPm) {
    // PM
    alarmHour = (uiHour == 12) ? 12 : (uiHour + 12);  // 12 PM→12, 1-11 PM→13-23
  } else {
    // AM
    alarmHour = (uiHour == 12) ? 0 : uiHour;  // 12 AM→0, 1-11 AM→1-11
  }

  // Build alarm time for today
  struct tm alarmTime = *now;
  alarmTime.tm_hour = alarmHour;
  alarmTime.tm_min = alarmMinute;
  alarmTime.tm_sec = 0;

  time_t alarmNextAlarm = mktime(&alarmTime);

  // If alarm has passed, add 24 hours
  if (alarmNextAlarm <= nowLong) {
    alarmNextAlarm += 86400;
  }

  Serial.printf("Setting next alarm to: %ld\n", alarmNextAlarm);
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_NEXT_ALARM, eez::IntegerValue(alarmNextAlarm));
}

void action_stop_alarm_sound(lv_event_t *e) {
  Serial.println(F("Stop Alarm"));
  alarmCurrentlyPlaying = false;
  Music_pause();
}

void action_play_timer_sound(lv_event_t *e) {
  Serial.println(F("Play Timer (async)"));

  unsigned long now = millis();
  if (timerCurrentlyPlaying || (now - lastTimerTrigger < TIMER_RETRIGGER_DELAY_MS)) {
    Serial.println(F(">> Ignoring rapid timer trigger"));
    return;
  }

  lastTimerTrigger = now;
  timerCurrentlyPlaying = true;

  int soundIndex = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_TIMER_SOUND).getInt();

  Serial.printf("Playing sound %d\n", soundIndex);
  playSound(soundIndex);  // This blocks for a few seconds
}

void action_stop_timer_sound(lv_event_t *e) {
  Serial.println(F("Stop Timer"));
  timerCurrentlyPlaying = false;
  Music_pause();
}

void action_update_alarm_timer_settings(lv_event_t *e) {
  serializableConfigs.write();
}

void AlarmTimer::serializeConfig(JsonDocument &doc) {
  JsonObject alarmObj = doc["alarm"].to<JsonObject>();
  alarmObj["alarm-sound"] = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_SOUND).getInt();
  alarmObj["active"] = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_ACTIVE).getBoolean();
  alarmObj["next-alarm"] = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_NEXT_ALARM).getInt();
  alarmObj["hour"] = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_HOUR).getInt();
  alarmObj["minute"] = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_MINUTE).getInt();
  alarmObj["am_pm"] = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_AM_PM).getInt();

  JsonObject timerObj = doc["timer"].to<JsonObject>();
  timerObj["timer-sound"] = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_TIMER_SOUND).getInt();
}

void AlarmTimer::deserializeConfig(JsonDocument &doc) {
  JsonObject alarmObj = doc["alarm"];
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_SOUND, eez::IntegerValue(alarmObj["alarm-sound"]));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_ACTIVE, eez::BooleanValue(alarmObj["active"]));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_NEXT_ALARM, eez::IntegerValue(alarmObj["next-alarm"]));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_HOUR, eez::IntegerValue(alarmObj["hour"]));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_MINUTE, eez::IntegerValue(alarmObj["minute"]));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ALARM_AM_PM, eez::IntegerValue(alarmObj["am_pm"]));

  JsonObject timerObj = doc["timer"];
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_TIMER_SOUND, eez::IntegerValue(timerObj["timer-sound"]));
}
