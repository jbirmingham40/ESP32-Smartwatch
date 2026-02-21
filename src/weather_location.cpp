#include <weather_location.h>
#include <geolocation.h>

extern GeoLocation ipLocation;

// assume this location is ip based
WeatherLocation::WeatherLocation() {
  // init the display str0
  snprintf(displayStr, sizeof(displayStr), "[ Use IP Location ]");
}

// assume this location is zip based
WeatherLocation::WeatherLocation::WeatherLocation(const char *zipcode) {
  loadUsingZipcode(zipcode);

  // init the display str
  snprintf(displayStr, sizeof(displayStr), "%s, %s (%s)", city, state, zipcode);
}

// load from stored settings
WeatherLocation::WeatherLocation(const char *zipcode, const char *city, const char *state, const char *latitude, const char *longitude)
  : GeoLocation(zipcode, city, state, latitude, longitude) {

  // init the display str
  snprintf(displayStr, sizeof(displayStr), "%s, %s (%s)", city, state, zipcode);
}

void WeatherLocation::copy(WeatherLocation &rhs) {
  strncpy(displayStr, rhs.displayStr, sizeof(displayStr));
  strncpy(latitude, rhs.latitude, sizeof(latitude));
  strncpy(longitude, rhs.longitude, sizeof(longitude));
  strncpy(zipcode, rhs.zipcode, sizeof(zipcode));
}

const char *WeatherLocation::getDisplayStr() {
  return displayStr;
}
