#include "src/eez-flow.h"
#include "src/screens.h"
#include "src/images.h"
#include "src/actions.h"
#include "src/ui.h"
#include "src/vars.h"
#include "widgets/lv_label.h"
#include "widgets/lv_img.h"
#include <cstdio>
#include <ctime>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <ArduinoJson.h>
#include "weather.h"
#include "weather_locations.h"
#include "geolocation.h"
#include "time_client.h"
#include "psram_alloc.h"

extern Weather weather;
extern WeatherLocations weatherLocations;
extern GeoLocation ipLocation;
extern TimeClient timeClient;

// External spinner control functions from main .ino file
extern void hideStartupSpinner();

bool weatherNeedsReload = true;

bool get_var_weather_needs_reload() {
  return weatherNeedsReload;
}

void set_var_weather_needs_reload(bool value) {
}

void action_update_weather_location(lv_event_t *e) {
  weatherNeedsReload = true;
}

const char *get_var_weather_icon_small() {
  const char *icon = weather.getSmallIcon();
  if (strlen(icon) == 0)
    return "02-small";  // return a default to prevent LVGL from crashing
  return icon;
}

void set_var_weather_icon_small(const char *value) {
}

const char *get_var_weather_icon_large() {
  const char *icon = weather.getLargeIcon();
  if (strlen(icon) == 0)
    return "02d@2x";  // return a default to prevent LVGL from crashing
  return icon;
}

void set_var_weather_icon_large(const char *value) {
}

const char *get_var_weather_city() {
  if(weatherLocations.getSelectedIndex() == 0) {
    return ipLocation.getCity();
  } else {
    return weatherLocations.getSelection().getCity();
  }
}

void set_var_weather_city(const char *value) {
}

// ============================================================================
// loadWeather() - Called from Core 0 (Periodic_Tasks)
// Fetches data and stores in pendingData struct, does NOT touch LVGL
// Thread-safe: Returns immediately if another call is already in progress
// ============================================================================
void Weather::loadWeather() {
  // Try to acquire the lock - if already locked, return immediately
  if (loadInProgress.test_and_set(std::memory_order_acquire)) {
    // Another thread is already loading weather, skip this call
    return;
  }

  // Use RAII-style cleanup to ensure flag is always cleared
  struct FlagClearer {
    std::atomic_flag &flag;
    ~FlagClearer() {
      flag.clear(std::memory_order_release);
    }
  } clearer{ loadInProgress };

  static unsigned long lastWeatherUpdate = 0;
  static long now = 0;

  now = millis();

  // reload the weather every 10 minutes or anytime weatherNeedsReload is true
  if (weatherNeedsReload || now - lastWeatherUpdate > 10 * 60 * 1000) {
    try {
      WeatherLocation weatherLocation = weatherLocations.getSelection();

      // Validate geolocation before proceeding - empty coordinates would cause API errors
      const char *lat;
      const char *lon;

      // we don't store the lat and long for the ip location because it changes
      if(weatherLocations.getSelectedIndex() == 0) {
        lat = ipLocation.getLatitude();
        lon = ipLocation.getLongitude();
      } else {
        lat = weatherLocation.getLatitude();
        lon = weatherLocation.getLongitude();
      }

      if (!lat || !lon || lat[0] == '\0' || lon[0] == '\0') {
        Serial.println(F(">> ERROR: Invalid weather location - cannot load weather"));
        Serial.printf(">> lat='%s' lon='%s'\n", lat ? lat : "(null)", lon ? lon : "(null)");
        // Don't update lastWeatherUpdate so we retry on next cycle
        return;
      }

      NetworkClientSecure *client = new NetworkClientSecure;
      // OPTIMIZATION: Skip cert verification to reduce internal heap usage from TLS buffers (~2-4KB)
      // open-meteo.com is a public weather API; data is not sensitive
      client->setInsecure();
      // NOTE: NetworkClientSecure does not expose setBufferSizes() — TLS record buffers
      // (~16KB) are allocated by mbedTLS via C malloc() and land on internal heap.
      // This is the unavoidable floor of internal heap usage during a weather fetch.

      // Track whether at least one load succeeded
      bool loadedSomething = false;

      Serial.println(F("Loading current and hourly weather..."));
      if (loadCurrentAndHourly(client, lat, lon)) {
        loadedSomething = true;
      } else {
        Serial.println(F(">> WARNING: Current/hourly weather load failed"));
      }
      
      Serial.println(F("Done loading hourly. Loading daily weather..."));
      if (loadDaily(client, lat, lon)) {
        loadedSomething = true;
      } else {
        Serial.println(F(">> WARNING: Daily weather load failed"));
      }
      
      Serial.println(F("... end loading daily weather"));
      delete client;
      
      // CRITICAL FIX: Only update timestamp if at least one call succeeded
      // This allows immediate retry on failure instead of waiting 10 minutes
      if (loadedSomething) {
        lastWeatherUpdate = now;
        weatherNeedsReload = false;
        newDataReady.store(true);
        Serial.println(F(">> Weather data loaded successfully - next update in 10 minutes"));
      } else {
        // Don't update lastWeatherUpdate - will retry on next check (30 seconds)
        Serial.println(F(">> ERROR: All weather loads failed - will retry in 30 seconds"));
        // Don't clear weatherNeedsReload either so retry happens
      }
    } catch (...) {
      Serial.println(">> EXCEPTION in loadWeather - will retry in 30 seconds");
      // Don't update lastWeatherUpdate on exception - allow retry
    }
  }
}

// ============================================================================
// applyToUI() - Called from Core 1 (main loop)
// Applies pendingData to LVGL - safe because we're on the LVGL thread
// Hides startup spinner on first weather load completion
// ============================================================================
void Weather::applyToUI() {
  if (!newDataReady.load()) {
    return;  // No new data to apply
  }

  Serial.println(F(">> Applying weather data to UI..."));

  // Current conditions
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_CURRENT_CONDITIONS,
                               eez::StringValue(pendingData.currentConditions));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_DATE,
                               eez::StringValue(pendingData.weatherDate));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_TEMPERATURE,
                               eez::IntegerValue(pendingData.temperature));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_WIND_SPEED,
                               eez::StringValue(pendingData.windSpeed));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_HUMIDITY_PERCENTAGE,
                               eez::StringValue(pendingData.humidityPct));
  eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_WEATHER_MIN_MAX,
                               eez::StringValue(pendingData.minMaxStr));

  // Copy icons to class members (for getLargeIcon/getSmallIcon)
  strncpy(weatherIconLarge, pendingData.iconLarge, sizeof(weatherIconLarge) - 1);
  weatherIconLarge[sizeof(weatherIconLarge) - 1] = '\0';
  strncpy(weatherIconSmall, pendingData.iconSmall, sizeof(weatherIconSmall) - 1);
  weatherIconSmall[sizeof(weatherIconSmall) - 1] = '\0';

  // Hourly forecast
  for (int i = 0; i < 8; i++) {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_HOUR_ICON_0 + i,
                                 eez::StringValue(pendingData.hourly[i].icon));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_HOUR_TEMP_0 + i,
                                 eez::StringValue(pendingData.hourly[i].temp));

    if (i > 0) {  // Skip first entry (NOW)
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_HOUR_LABEL_1 + (i - 1),
                                   eez::IntegerValue(pendingData.hourly[i].hour));
      eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_HOUR_AMPM_1 + (i - 1),
                                   eez::StringValue(pendingData.hourly[i].ampm));
    }
  }

  // Daily forecast
  for (int i = 0; i < 10; i++) {
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_DAY_FORECAST_0 + i,
                                 eez::StringValue(pendingData.daily[i].dayName));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_HIGH_TEMP_FORECAST_0 + i,
                                 eez::StringValue(pendingData.daily[i].high));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_LOW_TEMP_FORECAST_0 + i,
                                 eez::StringValue(pendingData.daily[i].low));
    eez::flow::setGlobalVariable(FLOW_GLOBAL_VARIABLE_ICON_FORECAST_0 + i,
                                 eez::StringValue(pendingData.daily[i].icon));
  }

  newDataReady.store(false);

  // Hide startup spinner on first weather load completion
  if (!isFirstWeatherLoaded()) {
    Serial.println(F(">> First weather load complete"));
    Serial.flush();  // Ensure output is sent before potentially crashing

    markFirstWeatherLoaded();

    // Serial.println(F(">> About to hide spinner..."));
    // Serial.flush();

    // hideStartupSpinner();

    // Serial.println(F(">> Spinner hidden successfully"));
    // Serial.flush();
  }

  Serial.println(F(">> Weather UI updated"));
}

// ============================================================================
// loadDaily() - Fetches daily forecast, stores in pendingData
// Returns true on success, false on failure
// ============================================================================
bool Weather::loadDaily(NetworkClientSecure *client, const char *lat, const char *lon) {
  HTTPClient https;

  Serial.print(F("[HTTPS] begin...\n"));
  char url[350] = { 0 };
  const int numResults = 10;
  snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&daily=temperature_2m_max,temperature_2m_min,weather_code&timezone=%s&forecast_days=%d&wind_speed_unit=mph&temperature_unit=fahrenheit",
           lat, lon, timeClient.getTimezoneName(), numResults);
  Serial.println(url);

  if (https.begin(*client, url)) {
    Serial.print(F("[HTTPS] GET...\n"));
    int httpCode = https.GET();

    if (httpCode > 0) {
      Serial.printf(F("[HTTPS] GET... code: %d\n"), httpCode);

      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        Serial.println(F("About to deserialize document"));

        // Read response into PSRAM buffer (avoids heap fragmentation)
        char *jsonBuffer = readResponseToPsram(https);
        if (!jsonBuffer) {
          Serial.println(F("Failed to read response"));
          https.end();
          return false;
        }

        JsonDocument doc(SpiRamAllocator::instance());
        DeserializationError error = deserializeJson(doc, jsonBuffer);
        // NOTE: Do NOT free jsonBuffer here! ArduinoJson uses zero-copy mode
        // and doc may contain pointers into jsonBuffer
        Serial.println(F("Done deserializing document"));

        if (error) {
          Serial.print(F("deserializeJson() failed: "));
          Serial.println(error.f_str());
          heap_caps_free(jsonBuffer);
          https.end();
          return false;
        }

        for (int i = 0; i < numResults; i++) {
          const float highTemp = doc["daily"]["temperature_2m_max"][i];
          const float lowTemp = doc["daily"]["temperature_2m_min"][i];
          const char *timeTemp = (const char *)doc["daily"]["time"][i];
          const int weatherCode = doc["daily"]["weather_code"][i];

          // Calculate day of week
          int y, m, d;
          sscanf(timeTemp, "%d-%d-%d", &y, &m, &d);
          int weekday = (d += m < 3 ? y-- : y - 2, 23 * m / 9 + d + 4 + y / 4 - y / 100 + y / 400) % 7;

          const char *dayNames[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
          strncpy(pendingData.daily[i].dayName, dayNames[weekday], sizeof(pendingData.daily[i].dayName) - 1);
          pendingData.daily[i].dayName[sizeof(pendingData.daily[i].dayName) - 1] = '\0';

          snprintf(pendingData.daily[i].high, sizeof(pendingData.daily[i].high), "%d°", (int)highTemp);
          snprintf(pendingData.daily[i].low, sizeof(pendingData.daily[i].low), "%d°", (int)lowTemp);
          snprintf(pendingData.daily[i].icon, sizeof(pendingData.daily[i].icon), "%02d-small",
                   convertOpenMeteoToOpenWeatherCode(weatherCode));

          // Also update class members for backward compatibility
          snprintf(dailyForecast[i].high, sizeof(dailyForecast[i].high), "%d°", (int)highTemp);
          snprintf(dailyForecast[i].low, sizeof(dailyForecast[i].low), "%d°", (int)lowTemp);
          snprintf(dailyForecast[i].icon, sizeof(dailyForecast[i].icon), "%02d-small",
                   convertOpenMeteoToOpenWeatherCode(weatherCode));
        }

        doc.clear();
        heap_caps_free(jsonBuffer);  // NOW safe to free
        https.end();
        return true;  // Success!
      }
    } else {
      Serial.printf(F("[HTTPS] GET... failed, error: %s\n"), https.errorToString(httpCode).c_str());
    }

    https.end();
  } else {
    Serial.printf(F("[HTTPS] Unable to connect\n"));
  }
  return false;  // Failed
}

// ============================================================================
// loadCurrentAndHourly() - Fetches current + hourly, stores in pendingData
// ============================================================================
bool Weather::loadCurrentAndHourly(NetworkClientSecure *client, const char *lat, const char *lon) {
  HTTPClient https;

  Serial.print(F("[HTTPS] begin...\n"));
  char url[350] = { 0 };
  snprintf(url, sizeof(url), "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s&daily=temperature_2m_max,temperature_2m_min,weather_code&hourly=temperature_2m,weather_code&current=temperature_2m,precipitation,weather_code&timezone=%s&forecast_days=3&wind_speed_unit=mph&temperature_unit=fahrenheit",
           lat, lon, timeClient.getTimezoneName());
  Serial.println(url);

  if (https.begin(*client, url)) {
    Serial.print(F("[HTTPS] GET...\n"));
    int httpCode = https.GET();

    if (httpCode > 0) {
      Serial.printf(F("[HTTPS] GET... code: %d\n"), httpCode);

      if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
        Serial.println(F("About to deserialize document"));

        // Read response into PSRAM buffer (avoids heap fragmentation)
        char *jsonBuffer = readResponseToPsram(https);
        if (!jsonBuffer) {
          Serial.println(F("Failed to read response"));
          https.end();
          return false;
        }

        JsonDocument doc(SpiRamAllocator::instance());
        DeserializationError error = deserializeJson(doc, jsonBuffer);
        // NOTE: Do NOT free jsonBuffer here! ArduinoJson uses zero-copy mode
        // and doc may contain pointers into jsonBuffer
        Serial.println(F("Done deserializing document"));

        if (error) {
          Serial.print(F("deserializeJson() failed: "));
          Serial.println(error.f_str());
          heap_caps_free(jsonBuffer);
          https.end();
          return false;
        }

        // Current conditions - store in pendingData
        int weatherCode = doc["current"]["weather_code"];
        const char *currentConditions = getCurrentCondsFromCode(weatherCode);
        strncpy(pendingData.currentConditions, currentConditions, sizeof(pendingData.currentConditions) - 1);
        pendingData.currentConditions[sizeof(pendingData.currentConditions) - 1] = '\0';

        struct tm timeinfo;
        getLocalTime(&timeinfo);

        strftime(pendingData.weatherDate, sizeof(pendingData.weatherDate), "%m. %d. %A", &timeinfo);

        pendingData.temperature = (int)doc["current"]["temperature_2m"];

        snprintf(pendingData.windSpeed, sizeof(pendingData.windSpeed), "%d m/h",
                 (int)doc["daily"]["wind_speed_10m_max"][0]);

        snprintf(pendingData.humidityPct, sizeof(pendingData.humidityPct), "%d%%",
                 (int)doc["current"]["precipitation"]);

        snprintf(pendingData.minMaxStr, sizeof(pendingData.minMaxStr), "Max: %d° Min: %d°",
                 (int)doc["daily"]["temperature_2m_max"][0],
                 (int)doc["daily"]["temperature_2m_min"][0]);

        // Icons
        snprintf(pendingData.iconLarge, sizeof(pendingData.iconLarge), "%02d%c@2x",
                 convertOpenMeteoToOpenWeatherCode(weatherCode),
                 timeinfo.tm_hour >= 6 && timeinfo.tm_hour <= 19 ? 'd' : 'n');
        snprintf(pendingData.iconSmall, sizeof(pendingData.iconSmall), "%02d-small",
                 convertOpenMeteoToOpenWeatherCode(weatherCode));

        // Hourly forecast
        const int numHours = 8;
        for (int i = 0; i < numHours; i++) {
          const int index = timeinfo.tm_hour + (i * 3);

          if (i != 0) {
            const char *timeTemp = (const char *)doc["hourly"]["time"][index];

            int year, month, day, hour;
            sscanf(timeTemp, "%d-%d-%dT%d:00", &year, &month, &day, &hour);

            if (hour == 0) {
              hour = 12;
              strncpy(pendingData.hourly[i].ampm, "AM", sizeof(pendingData.hourly[i].ampm) - 1);
              pendingData.hourly[i].ampm[sizeof(pendingData.hourly[i].ampm) - 1] = '\0';
            } else if (hour <= 11) {
              strncpy(pendingData.hourly[i].ampm, "AM", sizeof(pendingData.hourly[i].ampm) - 1);
              pendingData.hourly[i].ampm[sizeof(pendingData.hourly[i].ampm) - 1] = '\0';
            } else if (hour == 12) {
              strncpy(pendingData.hourly[i].ampm, "PM", sizeof(pendingData.hourly[i].ampm) - 1);
              pendingData.hourly[i].ampm[sizeof(pendingData.hourly[i].ampm) - 1] = '\0';
            } else {
              hour = hour - 12;
              strncpy(pendingData.hourly[i].ampm, "PM", sizeof(pendingData.hourly[i].ampm) - 1);
              pendingData.hourly[i].ampm[sizeof(pendingData.hourly[i].ampm) - 1] = '\0';
            }

            pendingData.hourly[i].hour = hour;
          }

          const int hourlyWeatherCode = doc["hourly"]["weather_code"][index];
          snprintf(pendingData.hourly[i].icon, sizeof(pendingData.hourly[i].icon), "%02d-small",
                   convertOpenMeteoToOpenWeatherCode(hourlyWeatherCode));

          const float temp = doc["hourly"]["temperature_2m"][index];
          snprintf(pendingData.hourly[i].temp, sizeof(pendingData.hourly[i].temp), "%d°", (int)temp);

          // Also update class members for backward compatibility
          snprintf(hourlyForecast[i].icon, sizeof(hourlyForecast[i].icon), "%02d-small",
                   convertOpenMeteoToOpenWeatherCode(hourlyWeatherCode));
          snprintf(hourlyForecast[i].temp, sizeof(hourlyForecast[i].temp), "%d°", (int)temp);
        }

        doc.clear();
        heap_caps_free(jsonBuffer);  // NOW safe to free
        https.end();
        return true;  // Success!
      }
    } else {
      Serial.printf(F("[HTTPS] GET... failed, error: %s\n"), https.errorToString(httpCode).c_str());
    }

    https.end();
  } else {
    Serial.printf(F("[HTTPS] Unable to connect\n"));
  }
  return false;  // Failed
}

int Weather::convertOpenMeteoToOpenWeatherCode(int code) {
  if (code == 0) {
    return 1;
  } else if (code >= 40 && code <= 49) {
    return 45;
  } else if (code >= 50 && code <= 59) {
    return 50;
  } else if (code >= 60 && code <= 69) {
    return 10;
  } else if (code >= 70 && code <= 79) {
    return 13;
  } else if (code >= 80 && code <= 89) {
    return 9;
  } else if (code >= 90 && code <= 99) {
    return 11;
  }
  return code;
}

const char *Weather::getCurrentCondsFromCode(int code) {
  switch (code) {
    case 0: return "Clear Sky";
    case 1: return "Mainly Clear";
    case 2: return "Partly Cloudy";
    case 3: return "Cloudy";
    case 45: return "Foggy";
    case 48: return "Rime Fog";
    case 51: return "Light Drizzle";
    case 53: return "Drizzle";
    case 55: return "Heavy Drizzle";
    case 56:
    case 57: return "Freezing Drizzle";
    case 61: return "Light Rain";
    case 63: return "Rain";
    case 65: return "Heavy Rain";
    case 66:
    case 67: return "Freezing Rain";
    case 71: return "Light Snow";
    case 73: return "Snow";
    case 75: return "Heavy Snow";
    case 77: return "Dusting";
    case 80: return "Light Showers";
    case 81: return "Showers";
    case 82: return "Heavy Showers";
    case 85: return "Light Snow";
    case 86: return "Snow Showers";
    case 95: return "Thunderstorm";
    case 96: return "Light Hail";
    case 99: return "Thunder/Hail";
    default: return "Unknown";
  }
}

bool Weather::isFirstWeatherLoaded() {
  return first_weather_loaded;
}

void Weather::markFirstWeatherLoaded() {
  first_weather_loaded = true;
}

// Helper: Read HTTP response into a PSRAM buffer to avoid heap fragmentation
// For known Content-Length: read directly into PSRAM (ZERO heap use)
// For chunked/unknown length: use getString() with small reserve
// Returns nullptr on failure, caller must free with heap_caps_free()
char *Weather::readResponseToPsram(HTTPClient &http, size_t maxSize) {
  // Check heap before allocating - need at least 25KB free for safety
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

    // Read with yielding: only consume stream->available() bytes per iteration so
    // readBytes() never spin-waits for data that hasn't arrived yet.  Yielding with
    // vTaskDelay lets IDLE0 run between TCP segments and feed the task watchdog.
    size_t bytesRead = 0;
    unsigned long readStart = millis();
    while (bytesRead < (size_t)contentLength && millis() - readStart < 30000) {
      int avail = stream->available();
      if (avail > 0) {
        int toRead = min(avail, (int)(contentLength - (int)bytesRead));
        size_t got = stream->readBytes(buffer + bytesRead, toRead);
        bytesRead += got;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    buffer[bytesRead] = '\0';

    if (bytesRead == 0) {
      heap_caps_free(buffer);
      Serial.println(F(">> No data read"));
      return nullptr;
    }

    Serial.printf(">> Read %d bytes directly to PSRAM\n", bytesRead);
    return buffer;
  }

  // For UNKNOWN content length (chunked transfer encoding), use http.getString()
  // which handles chunked decoding internally. A raw stream read does NOT decode
  // chunk-size markers, so the JSON parser receives garbage and fails.
  //
  // getString() allocates via Arduino String (malloc → internal heap), but:
  // - mbedtls_platform_set_calloc_free() already routes TLS buffers to PSRAM
  // - this String is temporary and freed when we return (no leak, no long-term fragmentation)
  // - the copy to PSRAM below ensures the caller's buffer is in PSRAM
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

  // Copy to PSRAM immediately — String freed on function return
  char *buffer = (char *)heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!buffer) {
    Serial.println(F(">> PSRAM alloc failed for chunked response copy"));
    return nullptr;
  }

  memcpy(buffer, response.c_str(), len + 1);
  Serial.printf(">> Read %d bytes (chunked) to PSRAM\n", len);
  return buffer;
}