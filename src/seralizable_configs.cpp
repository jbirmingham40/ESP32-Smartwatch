#include <serializable_configs.h>
#include <serializable_config.h>
#include <ArduinoJson.h>
#include "SD_Card.h"
#include "psram_alloc.h"

void SerializableConfigs::add(SerializableConfig &config) {
	serializableConfigs.push_back(&config);
}

void SerializableConfigs::write() {
	// create the JsonDocument
	JsonDocument doc(SpiRamAllocator::instance());

	for(int i=0; i<serializableConfigs.size(); i++) {
		serializableConfigs[i]->serializeConfig(doc);
	}

  File file = SD_MMC.open(filename, FILE_WRITE);
  if (!file) {
    Serial.println(F("Failed to open file"));
    return;
  }

	serializeJsonPretty(doc, file);
  file.close();
}

void SerializableConfigs::read() {
	JsonDocument doc(SpiRamAllocator::instance());

  File file = SD_MMC.open(filename);
  if (!file) {
    Serial.println(F("Failed to open file"));
    return;
  }

  DeserializationError error = deserializeJson(doc, file);
  if (error) {
    Serial.println(F("Error parsing JSON"));
    return;
  }

  file.close();

	for(int i=0; i<serializableConfigs.size(); i++) {
		serializableConfigs[i]->deserializeConfig(doc);
	}
}
