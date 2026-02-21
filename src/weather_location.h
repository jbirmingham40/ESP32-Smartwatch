#pragma once

#ifndef WEATHER_LOCATION_H
#define WEATHER_LOCATION_H

#include <cstdio>
#include <cstring>
#include <geolocation.h>

class WeatherLocation : public GeoLocation {
public:

  WeatherLocation();                     // this location is ip based
  WeatherLocation(const char *zipcode);  // this location is zip based
  WeatherLocation(const char *zipcode, const char *city, const char *state, const char *latitude, const char *longitude);
  const char *getDisplayStr();  // local

public:
  bool operator==(WeatherLocation const &rhs) {
    return strcmp(city, rhs.city) == 0;
  }
  void copy(WeatherLocation &rhs);

private:
  char displayStr[100] = { 0 };
};

#endif /*weather_location.h*/
