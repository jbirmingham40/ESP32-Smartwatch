#pragma once

#ifndef SETTINGS_H
#define SETTINGS_H

#include "src/actions.h"
#include "src/screens.h"
#include "src/vars.h"
#include "src/ui.h"
#include "serializable_config.h"

class Settings : public SerializableConfig {
  public:
    void setBrightness(int32_t brightness) { this->brightness = brightness; }
    int32_t getBrightness() { return brightness; }

    void setVolume(int32_t volume) { this->volume = volume; }
    int32_t getVolume() { return volume; }

    void setDailyStepsGoal(int32_t dailyStepsGoal) { this->dailyStepsGoal = dailyStepsGoal; }
    int32_t getDailyStepsGoal() { return dailyStepsGoal; }
  
    void serializeConfig(JsonDocument &doc) override;
    void deserializeConfig(JsonDocument &doc) override;

  private:
    int32_t brightness = 20;
    int32_t volume = 12;
    int32_t dailyStepsGoal = 15000;
};

#endif /* settings.h */