#include "src/eez-flow.h"
#include "src/ui.h"
#include "src/vars.h"
#include "geolocation.h"
#include "weather.h"  // For SpiRamAllocator singleton
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include "psram_alloc.h"
#include "secrets.h"

GeoLocation::GeoLocation() {
}

GeoLocation::GeoLocation(const char *zipcode) {
  loadUsingZipcode(zipcode);
}

GeoLocation::GeoLocation(const char *zipcode, const char *city, const char *state, const char *latitude, const char *longitude) {
  if(zipcode != NULL) strncpy(this->zipcode, zipcode, sizeof(this->zipcode));
  if(city != NULL) strncpy(this->city, city, sizeof(this->city));
  if(state != NULL) strncpy(this->state, state, sizeof(this->state));
  if(latitude != NULL) strncpy(this->latitude, latitude, sizeof(this->latitude));
  if(longitude != NULL) strncpy(this->longitude, longitude, sizeof(this->longitude));
}

bool GeoLocation::didIpChange() {
  // Skip if not connected to WiFi
  if (!WiFi.isConnected()) {
    Serial.println("didIpChange: WiFi not connected, skipping");
    return false;
  }

  bool ipChanged = false;

  static char lastIp[16] = { 0 };

  NetworkClientSecure *client = new NetworkClientSecure;
  if (client) {
    client->setCACert(we1RootCACert);

    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before NetworkClientSecure *client is
      HTTPClient https;
      Serial.print(F("[HTTPS] begin...\n"));

      if (https.begin(*client, "https://api.ipify.org")) {  // HTTPS
        Serial.print(F("[HTTPS] GET...\n"));
        // start connection and send HTTP header
        int httpCode = https.GET();

        // httpCode will be negative on error
        if (httpCode > 0) {
          // HTTP header has been send and Server response header has been handled
          Serial.printf(F("[HTTPS] GET... code: %d\n"), httpCode);

          // file found at server
          if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
            // Read IP address with explicit yields so IDLE0 can feed the TWDT.
            // Stream::timedRead() (used by readBytesUntil/readBytes/getString) is a
            // tight busy-loop on non-blocking sockets: it calls mbedtls_ssl_read()
            // rapidly, gets WANT_READ, and loops — never yielding, starving IDLE0
            // and triggering the task watchdog after 5 seconds.
            WiFiClient *stream = https.getStreamPtr();
            if (stream) {
              // Wait for body data to arrive with yields instead of spinning.
              unsigned long waitStart = millis();
              while (!stream->available() && millis() - waitStart < 5000) {
                vTaskDelay(pdMS_TO_TICKS(5));
              }
              char currentIp[16] = {0};
              size_t len = 0;
              // Read char-by-char; body is tiny (IP address) and all buffered once available().
              while (len < sizeof(currentIp) - 1 && stream->available()) {
                char c = (char)stream->read();
                if (c == '\n' || c == '\r') break;
                currentIp[len++] = c;
              }
              currentIp[len] = '\0';

              // Trim whitespace
              while (len > 0 && (currentIp[len-1] == '\r' || currentIp[len-1] == '\n' || currentIp[len-1] == ' ')) {
                currentIp[--len] = '\0';
              }
              
              if (strcmp(currentIp, lastIp) != 0) {
                strncpy(lastIp, currentIp, sizeof(lastIp) - 1);
                lastIp[sizeof(lastIp) - 1] = '\0';
                ipChanged = true;
                Serial.printf("IP changed to: %s\n", currentIp);
              } else {
                Serial.printf("IP unchanged: %s\n", currentIp);
              }
            }
          }
        } else {
          Serial.printf(F("[HTTPS] GET... failed, error: %s\n"), https.errorToString(httpCode).c_str());
          ipChanged = false;
        }

        https.end();
      } else {
        Serial.printf(F("[HTTPS] Unable to connect\n"));
        ipChanged = false;
      }

      // End extra scoping block
    }

    delete client;
  } else {
    Serial.println(F("Unable to create client"));
    ipChanged = false;
  }

  return ipChanged;
}

bool GeoLocation::loadUsingIp() {
  // Skip if not connected to WiFi
  if (!WiFi.isConnected()) {
    Serial.println("loadUsingIp: WiFi not connected, skipping");
    return false;
  }

  // dont refresh if our ip is the same
  if (!didIpChange()) return true;

  bool lookupSuccess = true;

  NetworkClientSecure *client = new NetworkClientSecure;
  if (client) {
    client->setCACert(isrgRootCACert);

    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before NetworkClientSecure *client is
      HTTPClient https;
      Serial.print(F("[HTTPS] begin...\n"));

      if (https.begin(*client, "https://api.ipgeolocation.io/v2/timezone?apiKey=" IPGEOLOCATION_API_KEY)) {  // HTTPS
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
            char *jsonBuffer = readResponseToPsram(https);
            if (!jsonBuffer) {
              Serial.println(F("Failed to read response"));
              lookupSuccess = false;
            } else {
              JsonDocument doc(SpiRamAllocator::instance());
              DeserializationError error = deserializeJson(doc, jsonBuffer);
              // NOTE: Do NOT free jsonBuffer here! ArduinoJson uses zero-copy mode
              // and doc may contain pointers into jsonBuffer
              Serial.println(F("Done deserializing document"));

              // Test if parsing succeeds.
              if (error) {
                Serial.print(F("deserializeJson() failed: "));
                Serial.println(error.f_str());
                lookupSuccess = false;
              } else {
                snprintf(latitude, sizeof(latitude), "%s", (const char *)doc["location"]["latitude"]);
                snprintf(longitude, sizeof(longitude), "%s", (const char *)doc["location"]["longitude"]);
                snprintf(city, sizeof(city), "%s", (const char *)doc["location"]["city"]);
                snprintf(state, sizeof(state), "%s", (const char *)doc["location"]["state_code"]);
              }

              // NOW safe to free - all data has been copied to class members
              heap_caps_free(jsonBuffer);
            }
          }
        } else {
          Serial.printf(F("[HTTPS] GET... failed, error: %s\n"), https.errorToString(httpCode).c_str());
          lookupSuccess = false;
        }

        https.end();
      } else {
        Serial.printf(F("[HTTPS] Unable to connect\n"));
        lookupSuccess = false;
      }

      // End extra scoping block
    }

    delete client;
  } else {
    Serial.println(F("Unable to create client"));
    lookupSuccess = false;
  }

  return lookupSuccess;
}

bool GeoLocation::loadUsingZipcode(const char *zipcode) {
  // Skip if not connected to WiFi
  if (!WiFi.isConnected()) {
    Serial.println("loadUsingZipcode: WiFi not connected, skipping");
    return false;
  }

  bool lookupSuccess = true;

  NetworkClientSecure *client = new NetworkClientSecure;
  if (client) {
    client->setCACert(we1RootCACert);

    {
      // Add a scoping block for HTTPClient https to make sure it is destroyed before NetworkClientSecure *client is
      HTTPClient https;
      Serial.print(F("[HTTPS] begin...\n"));

      char url[200];
      snprintf(url, 200, "https://api.zipcodestack.com/v1/search?apikey=" ZIPCODESTACK_API_KEY "&codes=%s&country=US", zipcode);
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
            char *jsonBuffer = readResponseToPsram(https);
            if (!jsonBuffer) {
              Serial.println(F("Failed to read response"));
              lookupSuccess = false;
            } else {
              JsonDocument doc(SpiRamAllocator::instance());
              DeserializationError error = deserializeJson(doc, jsonBuffer);
              // NOTE: Do NOT free jsonBuffer here! ArduinoJson uses zero-copy mode
              Serial.println(F("Done deserializing document"));

              // Test if parsing succeeds.
              if (error) {
                Serial.print(F("deserializeJson() failed: "));
                Serial.println(error.f_str());
                lookupSuccess = false;
              } else {
                JsonArray results = doc["results"][zipcode].as<JsonArray>();
                if (results && results.size() > 0) {
                  strncpy(this->zipcode, zipcode, sizeof(this->zipcode));
                  snprintf(this->latitude, sizeof(this->latitude), "%.4f", (float)results[0]["latitude"]);
                  snprintf(this->longitude, sizeof(this->longitude), "%.4f", (float)results[0]["longitude"]);
                  snprintf(this->city, sizeof(this->city), "%s", (const char *)results[0]["city"]);
                  snprintf(this->state, sizeof(this->state), "%s", (const char *)results[0]["state_code"]);
                } else {
                  // invalid zipcode do nothing
                  Serial.print(F("Invalid zipcode: "));
                  Serial.println(zipcode);
                  lookupSuccess = false;
                }
              }

              // NOW safe to free - all data has been copied to class members
              heap_caps_free(jsonBuffer);
            }
          }
        } else {
          Serial.printf(F("[HTTPS] GET... failed, error: %s\n"), https.errorToString(httpCode).c_str());
          lookupSuccess = false;
        }

        // ALWAYS call https.end() after https.begin() succeeded
        https.end();
      }

      delete client;
    }
  } else {
    Serial.println(F("Unable to create client"));
    lookupSuccess = false;
  }

  return lookupSuccess;
}

char *GeoLocation::readResponseToPsram(HTTPClient &http, size_t maxSize) {
  // Check heap before allocating - need at least 20KB free for safety
  size_t freeHeap = ESP.getFreeHeap();
  if (freeHeap < 25000) {
    Serial.printf(">> WARNING: Low heap (%d bytes), skipping HTTP read\n", freeHeap);
    return nullptr;
  }

  int contentLen = http.getSize();

  if (contentLen > (int)maxSize) {
    Serial.printf(">> Response too large: %d bytes (max %d)\n", contentLen, maxSize);
    return nullptr;
  }

  if (contentLen > 0) {
    // Known Content-Length: allocate exactly the right buffer and read with
    // explicit yields so IDLE0 can feed the task watchdog between TCP segments.
    char *buffer = (char *)heap_caps_malloc(contentLen + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buffer) {
      Serial.println(F(">> PSRAM alloc failed"));
      return nullptr;
    }

    WiFiClient *stream = http.getStreamPtr();
    size_t totalRead = 0;
    unsigned long readStart = millis();
    while (totalRead < (size_t)contentLen && millis() - readStart < 30000) {
      int avail = stream->available();
      if (avail > 0) {
        int toRead = min(avail, (int)(contentLen - (int)totalRead));
        size_t got = stream->readBytes(buffer + totalRead, toRead);
        totalRead += got;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));  // Yield — lets IDLE0 run and feed the TWDT
      }
    }
    buffer[totalRead] = '\0';

    if (totalRead == 0) {
      Serial.println(F(">> Empty HTTP response"));
      heap_caps_free(buffer);
      return nullptr;
    }

    Serial.printf(">> Read %d bytes to PSRAM (yielding read)\n", totalRead);
    return buffer;
  }

  // Unknown Content-Length (chunked transfer) — fall back to getString().
  // HTTPClient handles chunked-encoding decoding internally; raw stream reads
  // would expose chunk-size markers in the data.
  String response = http.getString();
  if (response.length() == 0) {
    Serial.println(F(">> Empty HTTP response"));
    return nullptr;
  }
  size_t len = response.length();
  if (len > maxSize) {
    Serial.printf(">> Response too large: %d bytes (max %d)\n", len, maxSize);
    return nullptr;
  }
  char *buffer = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    Serial.println(F(">> PSRAM alloc failed"));
    return nullptr;
  }
  memcpy(buffer, response.c_str(), len + 1);
  Serial.printf(">> Read %d bytes to PSRAM (chunked/getString)\n", len);
  return buffer;
}
