#include "weather_locations.h"
#include "weather_location.h"
#include "src/eez-flow.h"
#include "src/vars.h"
#include "geolocation.h"
#include "weather.h"
#include "src/actions.h"
#include "serializable_configs.h"
#include "serializable_config.h"

extern Weather weather;
extern GeoLocation ipLocation;
extern WeatherLocations weatherLocations;
extern SerializableConfigs serializableConfigs;

void action_load_weather_locations(lv_event_t *e) {
  weatherLocations.populateDropDown();
}

void action_create_weather_location(lv_event_t *e) {
  bool show_invalid_zipcode = false;
  if (weatherLocations.create() == false) {
    show_invalid_zipcode = true;
  }
  eez::flow::setUserProperty(0, eez::BooleanValue(show_invalid_zipcode));
}

void action_delete_weather_location(lv_event_t *e) {
  weatherLocations.remove();
}

int32_t get_var_weather_locations_selected_index() {
  return weatherLocations.getSelectedIndex();
}

void set_var_weather_locations_selected_index(int32_t value) {
  weatherLocations.setSelectedIndex(value);
}

WeatherLocations::WeatherLocations() {
  locationVector.push_back(WeatherLocation());
}

int WeatherLocations::getSelectedIndex() {
  return selectedIndex;
}

WeatherLocation &WeatherLocations::getSelection() {
  return locationVector[selectedIndex];
}

void WeatherLocations::setSelectedIndex(int selectedIndex) {
  this->selectedIndex = selectedIndex;

  // update our storage configuration
  serializableConfigs.write();
}

void WeatherLocations::populateDropDown() {
  Serial.println(F("populate drop down called"));

  // Create the dynamic array of availabile cities
  eez::ArrayOfString newArray(locationVector.size());
  for (uint32_t i = 0; i < locationVector.size(); i++) {
    newArray.value.getArray()->values[i] = eez::Value(locationVector[i].getDisplayStr(), eez::VALUE_TYPE_STRING);
  }
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_LOCATIONS, newArray);
}

bool WeatherLocations::create() {
  // convert the individual digits into a number
  int digit1 = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_LOCATION_DIGIT1).getInt();
  int digit2 = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_LOCATION_DIGIT2).getInt();
  int digit3 = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_LOCATION_DIGIT3).getInt();
  int digit4 = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_LOCATION_DIGIT4).getInt();
  int digit5 = eez::flow::getGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_LOCATION_DIGIT5).getInt();

  char zipcodeStr[6];
  snprintf(zipcodeStr, sizeof(zipcodeStr), "%d%d%d%d%d", digit1, digit2, digit3, digit4, digit5);

  WeatherLocation newWeatherLocation = WeatherLocation(zipcodeStr);
  if(strlen(newWeatherLocation.getZipcode()) == 0) return false; // no zip means it failed in the lookup

  // add the location
  locationVector.push_back(newWeatherLocation);

  // sort the list
  std::sort(locationVector.begin() + 1, locationVector.end(),
            [](WeatherLocation &a, WeatherLocation &b) {
              return strcmp(a.getCity(), b.getCity()) < 0;
            });

  // find where the new location ended up and make it the current selection
  auto it = std::find(locationVector.begin() + 1, locationVector.end(), newWeatherLocation);
  selectedIndex = it - locationVector.begin();

  // update our drop down list
  populateDropDown();

  // update our storage configuration
  serializableConfigs.write();

  return true;  // fix me, dont hardcode
}

void WeatherLocations::remove() {
  // delete the location
  locationVector.erase(locationVector.begin() + selectedIndex);

  // reset the selection to the first element
  selectedIndex = 0;

  // update our drop down list
  populateDropDown();

  // update our storage configuration
  serializableConfigs.write();
}

void WeatherLocations::serializeConfig(JsonDocument &doc) {
  JsonObject weatherDoc = doc["weather_locations"].to<JsonObject>();
  weatherDoc["selected_location"] = selectedIndex;

  JsonArray weatherLocArr = weatherDoc["locations"].to<JsonArray>();

  for (int i = 1; i < locationVector.size(); i++) {
    JsonObject weatherLocObj = weatherLocArr.add<JsonObject>();
    weatherLocObj["city"] = locationVector[i].getCity();
    weatherLocObj["state"] = locationVector[i].getState();
    weatherLocObj["zipcode"] = locationVector[i].getZipcode();
    weatherLocObj["latitude"] = locationVector[i].getLatitude();
    weatherLocObj["longitude"] = locationVector[i].getLongitude();
  }
}

void WeatherLocations::deserializeConfig(JsonDocument &doc) {
  selectedIndex = doc["weather_locations"]["selected_location"];

  for (JsonObject weatherLocArr : doc["weather_locations"]["locations"].as<JsonArray>()) {
    add(WeatherLocation(weatherLocArr["zipcode"], weatherLocArr["city"],
                        weatherLocArr["state"], weatherLocArr["latitude"], weatherLocArr["longitude"]));
  }
}