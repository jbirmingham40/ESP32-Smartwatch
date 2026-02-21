#include "src/ui.h"
#include "src/vars.h"
#include "time.h"
#include "time_client.h"
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "psram_alloc.h"
#include "secrets.h"
#include <stdlib.h>

TimeClient::TimeClient() {
}

void TimeClient::begin() {
  syncWithNTP();
}

void TimeClient::syncWithNTP() {
  struct tm timeinfo;

  Serial.println(F("Setting up time"));
  configTime(0, 0, "pool.ntp.org");  // First connect to NTP server, with 0 TZ offset
  if (!getLocalTime(&timeinfo)) {
    Serial.println(F("  Failed to obtain time"));
    return;
  }
  Serial.println(F("  Got the time from NTP"));
}

bool TimeClient::lookupTimezone(const char *latitude, const char *longitude) {
  bool lookupSuccess = true;

  NetworkClientSecure *client = new NetworkClientSecure;
  if (client) {
    client->setCACert(isrgRootCACert);

    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before NetworkClientSecure *client is
      HTTPClient https;
      Serial.print(F("[HTTPS] begin...\n"));

      char url[200];
      snprintf(url, 200, "https://api.timezonedb.com/v2.1/get-time-zone?key=" TIMEZONEDB_API_KEY "&by=position&lat=%s&lng=%s&format=json", latitude, longitude);
      Serial.println(url);

      if (https.begin(*client, url)) {  // HTTPS
        Serial.print(F("[HTTPS] GET...\n"));
        // start connection and send HTTP header
        int httpCode = https.GET();

        // httpCode will be negative on error
        if (httpCode > 0) {
          // HTTP header has been send and Server response header has been handled
          Serial.printf(F("[HTTPS] GET... code: %d\n"), httpCode);

          // file found at server
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            // Deserialize the JSON document
            Serial.println(F("About to deserialize document"));

            // Read response into PSRAM buffer (avoids heap fragmentation)
            char* jsonBuffer = readResponseToPsram(https);
            if (!jsonBuffer) {
              Serial.println(F("Failed to read response"));
              lookupSuccess = false;
            } else {
              JsonDocument doc(SpiRamAllocator::instance());
              DeserializationError error = deserializeJson(doc, jsonBuffer);
              heap_caps_free(jsonBuffer);  // Free PSRAM buffer immediately after parsing
              Serial.println(F("Done deserializing document"));

              // Test if parsing succeeds.
              if (error) {
                Serial.print(F("deserializeJson() failed: "));
                Serial.println(error.f_str());
                lookupSuccess = false;
              } else {
                /* {
                    "status": "OK",
                    "message": "",
                    "countryCode": "US",
                    "countryName": "United States",
                    "regionName": "Texas",
                    "cityName": "Lost Creek",
                    "zoneName": "America\/Chicago",
                    "abbreviation": "CST",
                    "gmtOffset": -21600,
                    "dst": "0",
                    "zoneStart": 1762066800,
                    "zoneEnd": 1772956800,
                    "nextAbbreviation": "CDT",
                    "timestamp": 1767356335,
                    "formatted": "2026-01-02 12:18:55"
                }*/
                if(doc["abbreviation"]) {
                  strncpy(timezoneName, doc["zoneName"], sizeof(timezoneName));
                  const char *abbr = doc["abbreviation"];
                  const char *nextAbbr = doc["nextAbbreviation"];
                  int gmtOffset = doc["gmtOffset"];
                  int offsetHours = abs(gmtOffset)/3600;

                  snprintf(timezone, sizeof(timezone), "%s%d%s", abbr, offsetHours, nextAbbr);   // CST6CDT
                  setenv("TZ", timezone, 1);  //  Now adjust the TZ.  Clock settings are adjusted to show the new local time
                  Serial.printf("    Setting timezone to %s and timezone name to %s\n", timezone, timezoneName);
                  tzset();
                } else {
                  // invalid timezone do nothing
                  Serial.println(F("Unable to load timezone"));
                  lookupSuccess = false;
                }
              }
            }
          }
          https.end();

        } else {
          Serial.printf(F("[HTTPS] Unable to connect\n"));
          lookupSuccess = false;
        }
      }

      delete client;
    }
  } else {
    Serial.println(F("Unable to create client"));
    lookupSuccess = false;
  }

  return lookupSuccess;
}

void TimeClient::refresh() {
  struct tm timeinfo;
  getLocalTime(&timeinfo);

  strftime(date_day_of_week, 4, "%a", &timeinfo);
  strftime(date_day_of_month, 7, "%d %b", &timeinfo);
  strftime(date_year, 5, "%Y", &timeinfo);

  time_sec = timeinfo.tm_sec;
  strftime(time_min, 3, "%M", &timeinfo);
  strftime(time_hour, 3, "%I", &timeinfo);
}

// Helper: Read HTTP response into a PSRAM buffer to avoid heap fragmentation
// For known Content-Length: read directly into PSRAM (ZERO heap use)
// For chunked/unknown length: use getString() with small reserve
// Returns nullptr on failure, caller must free with heap_caps_free()
char *TimeClient::readResponseToPsram(HTTPClient &http, size_t maxSize) {
  // Check heap before allocating - need at least 20KB free for safety
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 25000) {
    Serial.printf(">> WARNING: Low heap (%d bytes), skipping HTTP read\n", freeHeap);
    return nullptr;
  }

  WiFiClient *stream = http.getStreamPtr();
  if (!stream) {
    Serial.println(F(">> No stream available"));
    return nullptr;
  }

  int contentLength = http.getSize();

  // For KNOWN content length, read directly into PSRAM (ZERO heap use!)
  if (contentLength > 0) {
    if ((size_t)contentLength > maxSize) {
      Serial.printf(">> Response too large: %d bytes (max %d)\n", contentLength, maxSize);
      return nullptr;
    }

    char *buffer = (char *)heap_caps_malloc(contentLength + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
      Serial.println(F(">> PSRAM alloc failed for HTTP response"));
      return nullptr;
    }

    // Read directly into PSRAM buffer - NO heap use!
    size_t bytesRead = stream->readBytes(buffer, contentLength);
    buffer[bytesRead] = '\0';

    if (bytesRead == 0) {
      heap_caps_free(buffer);
      Serial.println(F(">> No data read"));
      return nullptr;
    }

    Serial.printf(">> Read %d bytes directly to PSRAM\n", bytesRead);
    return buffer;
  }

  // For UNKNOWN content length (chunked transfer), use getString()
  // Geolocation responses are small - typically under 1KB
  String response;
  response.reserve(2048);  // 2KB reserve - minimal heap impact
  response = http.getString();

  if (response.length() == 0) {
    Serial.println(F(">> Empty HTTP response"));
    return nullptr;
  }

  size_t len = response.length();
  if (len > maxSize) {
    Serial.printf(">> Response too large: %d bytes (max %d)\n", len, maxSize);
    return nullptr;
  }

  // Copy to PSRAM buffer immediately, String freed on return
  char *buffer = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    Serial.println(F(">> PSRAM alloc failed"));
    return nullptr;
  }

  memcpy(buffer, response.c_str(), len + 1);
  Serial.printf(">> Read %d bytes (chunked) to PSRAM\n", len);
  return buffer;
}
