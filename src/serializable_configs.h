#pragma once

#ifndef SERIALIZABLE_CONFIGS_H
#define SERIALIZABLE_CONFIGS_H

#include <ArduinoJson.h>
#include "serializable_config.h"

class SerializableConfigs {
	public:
		void add(SerializableConfig &);
		void write();
		void read();

	private:
		std::vector<SerializableConfig *> serializableConfigs;
		const char *filename = "/System/config.json";
};

#endif /* serializable_config.h */
