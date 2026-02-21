#include <unistd.h>
#include "Arduino.h"
#include "src/actions.h"
#include "src/eez-flow.h"
#include "src/vars.h"
#include "src/screens.h"
#include "src/ui.h"
#include "time.h"
#include "BAT_Driver.h"
#include "time_client.h"
#include "settings.h"

extern TimeClient timeClient;
extern Settings settings;


bool get_var_is_charging() {
  return BAT_Is_Charging();
}

void set_var_is_charging(bool value) {
}

const char *get_var_battery_charge_pct() {
  return BAT_Get_Charge_Percentage_Str();
}

void set_var_battery_charge_pct(const char *value) {
}

const char *get_var_date_day_of_week() {
  return timeClient.getDayOfWeek();
}

void set_var_date_day_of_week(const char *value) {
}

const char *get_var_date_day_of_month() {
  return timeClient.getDayOfMonth();
}

void set_var_date_day_of_month(const char *value) {
}

const char *get_var_date_year() {
  return timeClient.getYear();
}

void set_var_date_year(const char *value) {
}

extern int32_t get_var_time_secs() {
  return timeClient.getTimeSec();
}

extern void set_var_time_secs(int32_t value) {
}

extern const char *get_var_time_min() {
  return timeClient.getTimeMin();
}

extern void set_var_time_min(const char *value) {
}

extern const char *get_var_time_hour() {
  return timeClient.getTimeHour();
}

extern void set_var_time_hour(const char *value) {
}
