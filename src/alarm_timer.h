#pragma once

#ifndef ALARM_TIMER_H
#define ALARM_TIMER_H

#include "serializable_config.h"

class AlarmTimer : public SerializableConfig {

  public:
    void serializeConfig(JsonDocument &doc) override;
    void deserializeConfig(JsonDocument &doc) override;
};

#endif /* alarm_timer.h */