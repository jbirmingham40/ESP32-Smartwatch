#pragma once

#ifndef WEATHER_LOCATIONS_H
#define WEATHER_LOCATIONS_H

#include <weather_location.h>
#include <serializable_config.h>
#include <cstdio>
#include <cstring>
#include <vector>

class WeatherLocations : public SerializableConfig {
  public:
    WeatherLocations();
    void add(const WeatherLocation &loc) { locationVector.push_back(loc); }
    bool create();
    void remove();
    void populateDropDown();
    int getSelectedIndex();
    WeatherLocation &getSelection();
    void setSelectedIndex(int selectedIndex);

  public:
    void serializeConfig(JsonDocument &doc) override;
    void deserializeConfig(JsonDocument &doc) override;

  private:
    std::vector<WeatherLocation> locationVector;
    int selectedIndex = 0;
};

#endif /*weather_location.h*/
