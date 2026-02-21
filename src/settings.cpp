#include "Audio_PCM5101.h"
#include "Display_SPD2010.h"
#include "src/actions.h"
#include "src/screens.h"
#include "src/vars.h"
#include "src/ui.h"
#include "settings.h"
#include "serializable_configs.h"

extern Settings settings;
extern SerializableConfigs serializableConfigs;

int32_t get_var_settings_daily_steps_goal() {
  return settings.getDailyStepsGoal();
}

void set_var_settings_daily_steps_goal(int32_t value) {
  // lets round the values to the nearest 500
  value = (value + 250) / 500 * 500;
  settings.setDailyStepsGoal(value);
}

int32_t get_var_settings_volume() {
  //Serial.printf("Volume is %d\n", settings.getVolume());
  return settings.getVolume();
}

void set_var_settings_volume(int32_t value) {
  settings.setVolume(value);
  //Serial.printf("setting volume to %d\n", value);
  Volume_adjustment(value);
}

void action_play_volume_change_sound(lv_event_t *e) {
  Play_Music("/System/alarms", "alarm-clock-beep-988-2.wav");
  vTaskDelay(pdMS_TO_TICKS(1000));
  Music_pause();
}

int32_t get_var_settings_brightness() {
  //Serial.printf("Brightness is %d\n", settings.getBrightness());
  return settings.getBrightness();
}

void set_var_settings_brightness(int32_t value) {
  settings.setBrightness(value);
  Set_Backlight(value);
}

void action_settings_screen_load(lv_event_t *e) {
  lv_obj_set_ext_click_area(objects.brightness_slider, settings.getBrightness());
  lv_obj_set_ext_click_area(objects.volume_slider, settings.getVolume());
  lv_obj_set_ext_click_area(objects.daily_steps_goal_slider, settings.getDailyStepsGoal());
}

void action_settings_screen_unload(lv_event_t *e) {
  // update our storage configuration
  serializableConfigs.write();
}

void action_restart_smartwatch(lv_event_t *e) {
  esp_restart();
}

void Settings::serializeConfig(JsonDocument &doc) {
  JsonObject settingsDoc = doc["settings"].to<JsonObject>();
  settingsDoc["brightness"] = brightness;
  settingsDoc["volume"] = volume;
  settingsDoc["daily_steps_goal"] = dailyStepsGoal;
}

void Settings::deserializeConfig(JsonDocument &doc) {
  brightness = doc["settings"]["brightness"];
  volume = doc["settings"]["volume"];
  dailyStepsGoal = doc["settings"]["daily_steps_goal"];
}