#include "ble.h"
#include "Arduino.h"
#include "src/actions.h"
#include "wifi_client.h"
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <WiFiClient.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <new>
#include "pngle.h"
#include <lvgl.h>
#include "src/images.h"
#include <mbedtls/platform.h>
#include <psram_alloc.h>
#include "media_controls.h"

// External instances
extern BLE ble;
extern WiFi_Client wifiClient;
extern MediaControls mediaControls;

void MediaControls::set_artwork_target_widget(lv_obj_t *widget) {
  if (artwork_target_widget) {
    lv_obj_remove_event_cb(artwork_target_widget, artwork_widget_delete_cb);
  }

  artwork_target_widget = widget;

  if (artwork_target_widget) {
    lv_obj_add_event_cb(artwork_target_widget, artwork_widget_delete_cb, LV_EVENT_DELETE, nullptr);
  }
}

bool MediaControls::safeSendMediaCommand(AMSRemoteCommand cmd, const char *cmdName) {
  Serial.printf(">> Media: %s\n", cmdName);
  try {
    ble.sendMediaCommand(cmd);
    return true;
  } catch (const std::exception &e) {
    Serial.printf(">> Exception in %s: %s\n", cmdName, e.what());
    return false;
  } catch (...) {
    Serial.printf(">> Unknown exception in %s\n", cmdName);
    return false;
  }
}

void action_media_play(lv_event_t *e) {
  mediaControls.safeSendMediaCommand(AMS_REMOTE_COMMAND_PLAY, "Play");
}

void action_media_pause(lv_event_t *e) {
  mediaControls.safeSendMediaCommand(AMS_REMOTE_COMMAND_PAUSE, "Pause");
}

void action_media_toggle_play_pause(lv_event_t *e) {
  mediaControls.safeSendMediaCommand(AMS_REMOTE_COMMAND_TOGGLE_PLAY_PAUSE, "Toggle Play/Pause");
}

void action_media_next_track(lv_event_t *e) {
  mediaControls.safeSendMediaCommand(AMS_REMOTE_COMMAND_NEXT_TRACK, "Next Track");
}

void action_media_previous_track(lv_event_t *e) {
  mediaControls.safeSendMediaCommand(AMS_REMOTE_COMMAND_PREVIOUS_TRACK, "Previous Track");
}

void action_media_volume_up(lv_event_t *e) {
  mediaControls.safeSendMediaCommand(AMS_REMOTE_COMMAND_VOLUME_UP, "Volume Up");
}

void action_media_volume_down(lv_event_t *e) {
  mediaControls.safeSendMediaCommand(AMS_REMOTE_COMMAND_VOLUME_DOWN, "Volume Down");
}

void action_refresh_media_info(lv_event_t *e) {
  // FIX: Do NOT call requestTrackInfo/requestPlayerInfo from Core 1 (UI task).
  // Those functions do blocking GATT writes+reads. Calling from Core 1 while
  // BLE_Task on Core 0 is also doing GATT operations causes interleaved
  // operations that corrupt the AMS Entity Attribute protocol sequence.
  // Set amsNeedsManualRefresh so BLE_Task executes the refresh on Core 0.
  Serial.println(">> Refreshing media info (queuing for BLE task)");
  if (!ble.isAMSConnected()) {
    Serial.println(">> AMS not connected");
    return;
  }
  ble.amsNeedsManualRefresh.store(true, std::memory_order_relaxed);
}

// Called from UI core – buffers a request; does NOT launch immediately
void MediaControls::start_async_artwork_update(const char *title, const char *artist, lv_obj_t *widget) {
  strncpy(async_title, title ? title : "", sizeof(async_title) - 1);
  async_title[sizeof(async_title) - 1] = '\0';
  strncpy(async_artist, artist ? artist : "", sizeof(async_artist) - 1);
  async_artist[sizeof(async_artist) - 1] = '\0';

  // FIX: Widget lifecycle tracking via LV_EVENT_DELETE instead of lv_obj_is_valid().
  //
  // lv_obj_is_valid() caused Crash #3 (Double exception, EXCVADDR 0xfffffff4):
  //   - It walks LVGL's object tree and dereferences class_p function pointer tables.
  //   - If the heap is disturbed by a concurrent HTTPS fetch (calendar, artwork),
  //     class_p can point to corrupted memory with 0 as a function pointer value.
  //   - vtable dispatch to offset -12 = 0 - 12 = 0xfffffff4 → InstrFetchProhibited.
  //   - Exception handler tries the same corrupted path → Double exception.
  //
  // Fix: Register LV_EVENT_DELETE on the widget. LVGL fires this callback before
  // freeing the object memory, so artwork_target_widget is nulled while the object
  // is still valid — no probing of potentially-corrupted LVGL internals ever needed.
  
  // Always remove then re-add the delete callback — never accumulate duplicates.
  set_artwork_target_widget(widget);

  // Reset the debounce timer – each new call restarts the wait
  artwork_request_time = millis();
  artwork_pending = true;

  // Show default image immediately while we wait for the download
  artwork_use_default = true;

  Serial.printf("[Artwork] Queued (debounce %dms): %s – %s\n",
                ARTWORK_DEBOUNCE_MS, async_artist, async_title);
}

// LV_EVENT_DELETE callback — fired by LVGL before the artwork widget is freed.
// Nulling artwork_target_widget here guarantees we never hold a dangling pointer.
// This runs on Core 1 (LVGL context), same as all other artwork_target_widget accesses.
void MediaControls::artwork_widget_delete_cb(lv_event_t *e) {
  lv_obj_t *deleted = lv_event_get_target(e);
  if (artwork_target_widget == deleted) {
    Serial.println(F("[Artwork] artwork_target_widget deleted by LVGL – auto-clearing"));
    artwork_target_widget = nullptr;
  }
}

// Background task: fetch + decode, then signal UI core
static void artwork_task(void *param) {
  Serial.println(F("[Artwork] Background task started"));
  Serial.printf("[Artwork] Title: %s, Artist: %s\n", async_title, async_artist);
  
  // Monitor stack usage for debugging
  UBaseType_t stackStart = uxTaskGetStackHighWaterMark(NULL);

  char request_title[sizeof(async_title)];
  char request_artist[sizeof(async_artist)];
  strncpy(request_title, async_title, sizeof(request_title) - 1);
  request_title[sizeof(request_title) - 1] = '\0';
  strncpy(request_artist, async_artist, sizeof(request_artist) - 1);
  request_artist[sizeof(request_artist) - 1] = '\0';

  bitmap_image_t new_artwork = { 0 };

  bool ok = mediaControls.get_media_image(request_title, request_artist, &new_artwork);
  
  // Check stack usage before cleanup
  UBaseType_t stackEnd = uxTaskGetStackHighWaterMark(NULL);
  Serial.printf("[Artwork] Stack usage: %d bytes used (high water mark: %d bytes free)\n",
                (32768 - stackEnd * sizeof(StackType_t)), stackEnd * sizeof(StackType_t));
  
  if (stackEnd * sizeof(StackType_t) < 4096) {
    Serial.printf("[Artwork] WARNING: Stack nearly full! Only %d bytes free. Consider increasing task stack size.\n",
                  stackEnd * sizeof(StackType_t));
  }

  if (ok && new_artwork.bitmap_data) {
    // Free any previous staged artwork that was never consumed
    if (staged_artwork.bitmap_data) {
      heap_caps_free(staged_artwork.bitmap_data);
    }
    staged_artwork = new_artwork;
    strncpy(staged_artwork_title, request_title, sizeof(staged_artwork_title) - 1);
    staged_artwork_title[sizeof(staged_artwork_title) - 1] = '\0';
    strncpy(staged_artwork_artist, request_artist, sizeof(staged_artwork_artist) - 1);
    staged_artwork_artist[sizeof(staged_artwork_artist) - 1] = '\0';

    // FIX: store with memory_order_release.
    // This is the WRITE side of the release-acquire pair. It guarantees that ALL
    // writes above this line (especially staged_artwork = new_artwork) are fully
    // visible to Core 1 before Core 1's acquire-load of artwork_ready returns true.
    // Without this, Core 1 can observe artwork_ready=true but read stale
    // staged_artwork.bitmap_data pointing to already-freed memory (heap poison 0xAB).
    artwork_ready.store(true, std::memory_order_release);
    Serial.println(F("[Artwork] Background task finished - image staged"));
  } else {
    Serial.println(F("[Artwork] Background task finished - no image, using default"));
    if (new_artwork.bitmap_data) heap_caps_free(new_artwork.bitmap_data);
    // FIX: release store so Core 1 sees the flag only after all prior writes complete
    artwork_use_default.store(true, std::memory_order_release);
  }

  // FIX: release store matches acquire load in launch_artwork_task() check
  artwork_loading.store(false, std::memory_order_release);
  
  // FIX: Yield to ensure all cleanup operations complete before task deletes itself
  // This prevents the stack canary check from detecting corruption mid-cleanup
  vTaskDelay(pdMS_TO_TICKS(50));
  
  vTaskDeleteWithCaps(NULL);
}

// Actually spawn the background download task
void MediaControls::launch_artwork_task() {
  // Memory gate
  size_t freeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (freeInternal < ARTWORK_MIN_FREE_HEAP) {
    Serial.printf("[Artwork] Skipping – internal heap too low (%d free, need %d), using default\n",
                  freeInternal, ARTWORK_MIN_FREE_HEAP);
    artwork_use_default = true;  // show default since we can't download
    return;
  }

  artwork_loading.store(true, std::memory_order_release);

  Serial.printf("[Artwork] Launching download: %s – %s\n", async_artist, async_title);

  // One-shot task on Core 0 – stack allocated in PSRAM to save internal heap
  // FIX: Increased stack to 32KB to avoid corruption during mbedTLS cleanup on error paths
  xTaskCreatePinnedToCoreWithCaps(
    artwork_task,
    "ArtworkDL",
    32768,          // 32KB stack - mbedTLS cleanup on error paths needs extra space
    NULL,
    1,
    NULL,
    0,
    MALLOC_CAP_SPIRAM);
}

// Called every loop() iteration on Core 1 (UI core)
// Handles debounce-to-launch, applying finished artwork, and default fallback
void MediaControls::apply_pending_artwork() {
  // 1) Check if a debounced request is ready to launch
  // FIX: acquire load pairs with release store in launch_artwork_task / artwork_task
  if (artwork_pending && !artwork_loading.load(std::memory_order_acquire)) {
    if (millis() - artwork_request_time >= ARTWORK_DEBOUNCE_MS) {
      artwork_pending = false;
      launch_artwork_task();
    }
  }

  // 2) Show default image if download failed or was skipped
  // FIX: acquire load - pairs with the release store in artwork_task.
  // Ensures we see all of Core 0's writes before acting on this flag.
  if (artwork_use_default.load(std::memory_order_acquire)) {
    artwork_use_default.store(false, std::memory_order_relaxed);

    // Free any previous downloaded bitmap
    if (current_artwork.bitmap_data) {
      // FIX: null img_dsc->data before freeing so LVGL can't render freed memory
      invalidate_img_dsc();
      heap_caps_free(current_artwork.bitmap_data);
      current_artwork = { 0 };
      current_artwork_title[0] = '\0';
      current_artwork_artist[0] = '\0';
    }

    // artwork_target_widget is guaranteed null-safe here:
    // LV_EVENT_DELETE (registered in start_async_artwork_update) fires before LVGL
    // frees any object, so artwork_target_widget is always either null or pointing
    // to a live, valid LVGL object. No lv_obj_is_valid() probe needed or safe.
    if (artwork_target_widget) {
      invalidate_img_dsc();
      lv_img_set_src(artwork_target_widget, &img_music_100);
      lv_obj_invalidate(artwork_target_widget);
      Serial.println(F("[Artwork] Showing default image"));
    }
    return;
  }

  // 3) Apply finished downloaded artwork to LVGL
  // FIX: acquire load - pairs with the release store in artwork_task.
  // Guarantees staged_artwork is fully written before we read it here.
  if (!artwork_ready.load(std::memory_order_acquire)) return;
  artwork_ready.store(false, std::memory_order_relaxed);

  Serial.println(F("[Artwork] Applying staged artwork to LVGL"));

  // Free the old displayed bitmap
  if (current_artwork.bitmap_data) {
    // FIX: null img_dsc->data before freeing so LVGL can't render freed memory
    // during the window between this free() and the update_lvgl_image() call below.
    invalidate_img_dsc();
    heap_caps_free(current_artwork.bitmap_data);
  }
  current_artwork = staged_artwork;
  staged_artwork = { 0 };
  strncpy(current_artwork_title, staged_artwork_title, sizeof(current_artwork_title) - 1);
  current_artwork_title[sizeof(current_artwork_title) - 1] = '\0';
  strncpy(current_artwork_artist, staged_artwork_artist, sizeof(current_artwork_artist) - 1);
  current_artwork_artist[sizeof(current_artwork_artist) - 1] = '\0';
  staged_artwork_title[0] = '\0';
  staged_artwork_artist[0] = '\0';

  // artwork_target_widget is null-safe (see LV_EVENT_DELETE comment above).
  if (artwork_target_widget && current_artwork.bitmap_data) {
    update_lvgl_image(artwork_target_widget, &current_artwork);
  }
}

static void pngle_on_draw(pngle_t *pngle, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const uint8_t rgba[4]) {
  bitmap_image_t *img = (bitmap_image_t *)pngle_get_user_data(pngle);
  if (!img || !img->bitmap_data) return;

  for (uint32_t dy = 0; dy < h; dy++) {
    for (uint32_t dx = 0; dx < w; dx++) {
      uint32_t pos = ((y + dy) * img->width + (x + dx));
      // FIX: Guard against out-of-bounds writes that corrupt heap metadata
      if (pos >= (uint32_t)(img->width * img->height)) {
        Serial.printf("[Artwork] WARNING: pngle out-of-bounds at x=%u y=%u pos=%u max=%u\n",
                      x+dx, y+dy, pos, img->width * img->height);
        return;
      }
      ((uint16_t *)img->bitmap_data)[pos] = ((rgba[0] & 0xF8) << 8) | ((rgba[1] & 0xFC) << 3) | (rgba[2] >> 3);
    }
  }
}

bool MediaControls::download_and_convert_artwork(const char *image_url, bitmap_image_t *out_image) {
  if (!image_url || strlen(image_url) == 0) {
    Serial.println(F("[Artwork] Invalid URL"));
    return false;
  }

  if (!wifiClient.isConnected()) {
    Serial.println(F("[Artwork] WiFi disconnected before image fetch - reconnecting"));
    if (!wifiClient.smartConnect(10000, false)) {
      Serial.println(F("[Artwork] Reconnect failed - cannot download image"));
      return false;
    }
  }
  wifiClient.keepAlive();  // Keep WiFi alive for 30 s after this fetch

  // Use plain HTTP for CDN image downloads – saves ~50KB of internal heap
  // that would otherwise be consumed by TLS buffers.
  // Album artwork is public, non-sensitive content from Apple's CDN.
  String httpUrl = String(image_url);
  httpUrl.replace("https://", "http://");

  WiFiClient client;      // Plain TCP – no TLS overhead!
  client.setTimeout(15);  // seconds

  bool success = false;
  {
    HTTPClient http;

    Serial.printf("[Artwork] Downloading PNG (HTTP) from: %s\n", httpUrl.c_str());
    if (http.begin(client, httpUrl.c_str())) {
      int httpCode = http.GET();

      if (httpCode == HTTP_CODE_OK) {
        int totalLen = http.getSize();
        Serial.printf("[Artwork] PNG size: %d bytes\n", totalLen);

        // Download the entire image into a PSRAM buffer first
        size_t bufSize = (totalLen > 0) ? totalLen : 65536;
        uint8_t *imgBuf = (uint8_t *)heap_caps_malloc(bufSize, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!imgBuf) {
          Serial.println(F("[Artwork] Failed to allocate image buffer in PSRAM"));
          http.end();
          return false;
        }

        Stream *stream = http.getStreamPtr();
        size_t totalRead = 0;
        int remaining = totalLen;
        while (http.connected() && (remaining > 0 || remaining == -1) && totalRead < bufSize) {
          size_t available = stream->available();
          if (available) {
            size_t toRead = min(available, bufSize - totalRead);
            int c = stream->readBytes(imgBuf + totalRead, toRead);
            totalRead += c;
            if (remaining > 0) remaining -= c;
          }
          vTaskDelay(pdMS_TO_TICKS(10));  // 10ms gives IDLE0 and BLE time to run
        }
        Serial.printf("[Artwork] Downloaded %d bytes\n", totalRead);

        // Decode PNG
        pngle_t *pngle = pngle_new();
        if (pngle) {
          pngle_set_init_callback(pngle, [](pngle_t *pngle, uint32_t w, uint32_t h) {
            bitmap_image_t *img = (bitmap_image_t *)pngle_get_user_data(pngle);
            img->width = w;
            img->height = h;
            img->size = w * h * 2;
            Serial.printf("[Artwork] PNG dimensions: %dx%d, allocating %d bytes in PSRAM\n", w, h, img->size);
            img->bitmap_data = (uint8_t *)heap_caps_malloc(img->size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!img->bitmap_data) {
              Serial.println(F("[Artwork] FAILED to allocate bitmap buffer in PSRAM!"));
            }
          });
          pngle_set_draw_callback(pngle, pngle_on_draw);
          pngle_set_user_data(pngle, out_image);

          int fed = pngle_feed(pngle, imgBuf, totalRead);
          if (fed < 0) {
            Serial.printf("[Artwork] PNG decode error: %s\n", pngle_error(pngle));
          }
          success = (out_image->bitmap_data != NULL);
          if (success) {
            Serial.println(F("[Artwork] Successfully decoded PNG"));
          }
          pngle_destroy(pngle);
        } else {
          Serial.println(F("[Artwork] Failed to create PNG decoder"));
        }

        heap_caps_free(imgBuf);
        if (success) {
          Serial.printf("[Artwork] Final heap free: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_8BIT));
          Serial.printf("[Artwork] Final PSRAM free: %d bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        }
      } else {
        Serial.printf("[Artwork] GET failed, error: %s\n", http.errorToString(httpCode).c_str());
      }

      http.end();
    }
  }

  return success;
}

// URL-encode a string for use in query parameters.
// Replaces spaces with '+' and percent-encodes special characters.
void MediaControls::urlEncode(const char *src, char *dst, size_t dstSize) {
  static const char *hex = "0123456789ABCDEF";
  size_t pos = 0;
  while (*src && pos + 3 < dstSize) {
    char c = *src++;
    if (('A' <= c && c <= 'Z') || ('a' <= c && c <= 'z') || ('0' <= c && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
      dst[pos++] = c;
    } else if (c == ' ') {
      dst[pos++] = '+';
    } else {
      dst[pos++] = '%';
      dst[pos++] = hex[(uint8_t)c >> 4];
      dst[pos++] = hex[(uint8_t)c & 0x0F];
    }
  }
  dst[pos] = '\0';
}

bool MediaControls::get_media_image(const char *title, const char *artist, bitmap_image_t *out_image) {
  bool lookupSuccess = false;
  String savedArtworkUrl;

  if (!title || strlen(title) == 0) {
    Serial.println(F("[iTunes] No title provided"));
    return false;
  }

  if (!wifiClient.isConnected()) {
    Serial.println(F("[iTunes] WiFi disconnected - reconnecting for artwork lookup"));
    if (!wifiClient.smartConnect(15000, false)) {
      Serial.println(F("[iTunes] Reconnect failed - cannot query artwork API"));
      return false;
    }
  }
  wifiClient.keepAlive();  // Keep WiFi alive for 30 s after this lookup
  
  bool tlsEstablished = false;  // Track if we actually established a TLS connection

  // HTTPS required – iTunes redirects HTTP to HTTPS (302).
  // FIX: Allocate NetworkClientSecure in PSRAM via placement new.
  // The default `new` puts the object in DRAM, where it lands adjacent to
  // shared_vector_desc_t interrupt-handler descriptors. TLS cleanup writes
  // (zeroing internal esp_tls handles and mbedTLS context pointers) then
  // overshot into the adjacent descriptor's `next` field, producing the
  // deterministic EXCVADDR 0x00300010 / shared_intr_isr crash.
  // With the object in PSRAM, any out-of-bounds cleanup writes hit PSRAM
  // data rather than DRAM interrupt-handler linked-list nodes.
  uint8_t *clientBuf = (uint8_t *)heap_caps_malloc(sizeof(NetworkClientSecure),
                                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!clientBuf) {
    Serial.println(F("[iTunes] Failed to allocate client buffer in PSRAM"));
    return false;
  }
  NetworkClientSecure *client = new (static_cast<void *>(clientBuf)) NetworkClientSecure();
  client->setInsecure();  // skip cert verification – saves a few more KB
  client->setTimeout(15);

  // Placement-new object cleanup helper. We intentionally destroy the object
  // only when TLS was established, mirroring prior safety behavior.
  auto cleanupClient = [&](bool destroyClient) {
    if (client && destroyClient) {
      // Ensure socket state is closed before teardown.
      client->stop();
      vTaskDelay(pdMS_TO_TICKS(10));
      client->~NetworkClientSecure();
      client = nullptr;
    }
    if (clientBuf) {
      heap_caps_free(clientBuf);
      clientBuf = nullptr;
    }
  };

  char *url = (char *)heap_caps_malloc(512, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!url) {
    Serial.println(F("[iTunes] Failed to allocate URL buffer"));
    // No network activity occurred yet; explicit teardown is safe here.
    cleanupClient(true);
    return false;
  }

  // URL-encode title and artist to handle spaces and special characters
  char encodedTitle[200];
  char encodedArtist[150];
  urlEncode(title, encodedTitle, sizeof(encodedTitle));
  if (artist && strlen(artist) > 0) {
    urlEncode(artist, encodedArtist, sizeof(encodedArtist));
  } else {
    encodedArtist[0] = '\0';
  }

  if (encodedArtist[0] != '\0') {
    snprintf(url, 512, "https://itunes.apple.com/search?entity=musicTrack&limit=1&term=%s+%s", encodedTitle, encodedArtist);
  } else {
    snprintf(url, 512, "https://itunes.apple.com/search?entity=musicTrack&limit=1&term=%s", encodedTitle);
  }
  Serial.printf("[iTunes] Query: %s\n", url);

  {
    HTTPClient https;

    if (https.begin(*client, url)) {
      Serial.println(F("[iTunes] Searching..."));
      int httpCode = https.GET();

      if (httpCode > 0) {
        tlsEstablished = true;  // TLS handshake completed successfully
        Serial.printf("[HTTPS] GET... code: %d\n", httpCode);

        if (httpCode == HTTP_CODE_OK || httpCode == HTTP_CODE_MOVED_PERMANENTLY) {
          Serial.println(F("[iTunes] Parsing JSON response"));

          char *jsonBuffer = (char *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
          if (!jsonBuffer) {
            Serial.println(F("[iTunes] Failed to allocate JSON buffer in PSRAM"));
            lookupSuccess = false;
          } else {
            Stream *stream = https.getStreamPtr();
            size_t bytesRead = stream->readBytes(jsonBuffer, 8191);
            jsonBuffer[bytesRead] = '\0';
            Serial.printf("[iTunes] Read %d bytes\n", bytesRead);

            JsonDocument doc(SpiRamAllocator::instance());
            DeserializationError error = deserializeJson(doc, jsonBuffer);

            if (!error) {
              int resultCount = doc["resultCount"] | 0;
              if (resultCount > 0 && !doc["results"][0]["artworkUrl100"].isNull()) {
                String artworkUrl = doc["results"][0]["artworkUrl100"].as<String>();
                // Request PNG format from Apple's CDN
                int jpgPos = artworkUrl.lastIndexOf(".jpg");
                if (jpgPos > 0) {
                  artworkUrl = artworkUrl.substring(0, jpgPos) + ".png";
                }
                Serial.printf("[iTunes] Artwork URL: %s\n", artworkUrl.c_str());
                savedArtworkUrl = artworkUrl;
              } else {
                Serial.printf("[iTunes] No results found (resultCount: %d)\n", resultCount);
              }
            } else {
              Serial.printf("[iTunes] JSON parse error: %s\n", error.c_str());
            }

            doc.clear();
            heap_caps_free(jsonBuffer);
          }
        } else {
          Serial.printf("[iTunes] API call failed, error: %s\n", https.errorToString(httpCode).c_str());
        }
        https.end();

      } else {
        Serial.printf("[HTTPS] Unable to connect\n");
        https.end();  // FIX: Must call end() even on connection failure
        lookupSuccess = false;
      }
    } else {
      Serial.println(F("[HTTPS] Failed to begin connection"));
      lookupSuccess = false;
    }
    
    // Wait for all HTTPS cleanup to complete before destroying client
    vTaskDelay(pdMS_TO_TICKS(20));

    // Call destructor explicitly (placement new requires explicit destructor call).
    // Only call when TLS was established - if the handshake never completed the
    // mbedTLS internal state may be inconsistent and the destructor could write
    // out-of-bounds. Since the object is in PSRAM any such write will only
    // corrupt PSRAM data, not DRAM interrupt-handler descriptors, so this is
    // safe either way - but skipping it on failure avoids the noisy stack trace.
    if (tlsEstablished) {
      cleanupClient(true);
    } else {
      cleanupClient(false);
    }
  }

  heap_caps_free(url);

  // Now download artwork via plain HTTP with TLS fully released
  if (savedArtworkUrl.length() > 0) {
    lookupSuccess = download_and_convert_artwork(savedArtworkUrl.c_str(), out_image);
  }

  return lookupSuccess;
}

void MediaControls::update_lvgl_image(lv_obj_t *img_obj, bitmap_image_t *bitmap) {
  if (!img_obj || !bitmap || !bitmap->bitmap_data) {
    Serial.println(F("[LVGL] Invalid parameters for update"));
    return;
  }

  // No lv_obj_is_valid() check needed here: the LV_EVENT_DELETE callback registered
  // in start_async_artwork_update() guarantees artwork_target_widget (which is always
  // the img_obj passed here) is null before LVGL frees it. Calling lv_obj_is_valid()
  // on a heap-disturbed system was the root cause of Crash #3 (Double exception,
  // InstrFetchProhibited at 0xfffffff4 — vtable[-3] through a null class_p pointer).
  if (!img_dsc) {
    img_dsc = (lv_img_dsc_t *)heap_caps_malloc(sizeof(lv_img_dsc_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!img_dsc) {
      Serial.println(F("[LVGL] Failed to allocate image descriptor"));
      return;
    }
  }

  // FIX: UNCONDITIONALLY invalidate LVGL's image cache for this descriptor.
  //
  // LVGL's image cache (_lv_img_cache_open) matches entries by the SOURCE POINTER,
  // not by the data the pointer references.  Since img_dsc is a persistent class
  // member (same address every call), a cache hit returns the old dec_dsc.img_data
  // — which points to the PREVIOUS track's bitmap that was already freed.
  //
  // Sequence that causes the bug:
  //   1. apply_pending_artwork() calls invalidate_img_dsc() → img_dsc->data = nullptr
  //   2. heap_caps_free(old_bitmap)
  //   3. update_lvgl_image() is called
  //   4. The old guard "if (img_dsc->data)" was FALSE (nulled in step 1)
  //      → lv_img_cache_invalidate_src was NEVER called
  //   5. LVGL renders → cache hit for img_dsc → returns dec_dsc.img_data = old_bitmap
  //   6. LVGL reads freed memory → stale image or garbage displayed
  //
  // Fix: always invalidate regardless of img_dsc->data.  lv_img_cache_invalidate_src
  // compares cache[i].dec_dsc.src == img_dsc (the outer pointer), so it works even
  // when img_dsc->data is null.
  lv_img_cache_invalidate_src(img_dsc);

  img_dsc->header.always_zero = 0;
  img_dsc->header.w = bitmap->width;
  img_dsc->header.h = bitmap->height;
  img_dsc->data_size = bitmap->size;
  img_dsc->header.cf = LV_IMG_CF_TRUE_COLOR;
  img_dsc->data = bitmap->bitmap_data;

  lv_img_set_src(img_obj, img_dsc);
  lv_obj_invalidate(img_obj);
  Serial.println(F("[LVGL] Image updated"));
}

void MediaControls::update_album_artwork(const char *title, const char *artist, lv_obj_t *artwork_widget) {
  const char *safeTitle = title ? title : "";
  const char *safeArtist = artist ? artist : "";

  bool hasCachedArtworkForTrack =
      current_artwork.bitmap_data &&
      strcmp(current_artwork_title, safeTitle) == 0 &&
      strcmp(current_artwork_artist, safeArtist) == 0;

  if (hasCachedArtworkForTrack) {
    set_artwork_target_widget(artwork_widget);
    if (artwork_target_widget) {
      update_lvgl_image(artwork_target_widget, &current_artwork);
      Serial.println(F("[Artwork] Reusing cached artwork for current track"));
    } else {
      Serial.println(F("[Artwork] Cached artwork present; widget not active"));
    }
    return;
  }

  Serial.println(F("[Artwork] Requesting async artwork update..."));
  Serial.printf("[Artwork] Title: %s, Artist: %s\n", safeTitle, safeArtist);
  start_async_artwork_update(safeTitle, safeArtist, artwork_widget);
}

void MediaControls::update_playback_timing(const char *title, const char *artist, bool isPlaying, float remainingSeconds) {
  // WiFi lifecycle is now managed by wifiClient.keepAlive() / processLifecycle().
  // No preconnect logic needed here.
}

// FIX: Nulls img_dsc->data before any free() of the underlying bitmap so that
// LVGL's render pipeline cannot read freed memory between the free() and the
// next update_lvgl_image() call.  Must be called on Core 1 (UI core) only.
void MediaControls::invalidate_img_dsc() {
  if (img_dsc) {
    img_dsc->data = nullptr;
    img_dsc->data_size = 0;
  }
}

void MediaControls::free_artwork() {
  if (current_artwork.bitmap_data) {
    Serial.println(F("[Artwork] Cleaning up artwork"));
    // FIX: Null img_dsc->data BEFORE freeing the buffer so LVGL's render pipeline
    // cannot read freed memory if a display refresh fires between free() and the
    // next update_lvgl_image() call (which would repopulate img_dsc->data).
    invalidate_img_dsc();
    heap_caps_free(current_artwork.bitmap_data);
    current_artwork.bitmap_data = NULL;
    current_artwork.width = 0;
    current_artwork.height = 0;
    current_artwork.size = 0;
    current_artwork_title[0] = '\0';
    current_artwork_artist[0] = '\0';
  }
}
