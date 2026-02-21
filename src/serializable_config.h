#pragma once

#ifndef SERIALIZABLE_CONFIG_H
#define SERIALIZABLE_CONFIG_H

#include <ArduinoJson.h>

class SerializableConfig {
	public:
		virtual void serializeConfig(JsonDocument &doc) = 0;
		virtual void deserializeConfig(JsonDocument &doc) = 0;
};

#endif /* serializable_config.h */
