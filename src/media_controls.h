#pragma once

#ifndef MEDIA_CONTROLS_H
#define MEDIA_CONTROLS_H

#include <atomic>  // FIX: Required for std::atomic<bool> cross-core signaling

#define ARTWORK_MIN_FREE_HEAP 35000
#define ARTWORK_DEBOUNCE_MS 500  // wait for AMS fields to settle

typedef struct {
  uint8_t *bitmap_data;
  uint16_t width;
  uint16_t height;
  size_t size;
} bitmap_image_t;

static bitmap_image_t current_artwork = { 0 };

// Staging buffer: background task (Core 0) writes here, UI core (Core 1) reads from here.
static bitmap_image_t staged_artwork = { 0 };

// FIX: Changed from 'volatile bool' to 'std::atomic<bool>'.
//
// On ESP32 dual-core Xtensa, 'volatile' prevents compiler reordering but does NOT
// prevent CPU-level write reordering or cache-line incoherence between cores.
// Without proper memory ordering, the following race is possible:
//
//   Core 0 (artwork_task):
//     staged_artwork = new_artwork;   // written to Core 0's cache line
//     artwork_ready = true;           // Core 1 sees this SET immediately...
//
//   Core 1 (apply_pending_artwork):
//     if (artwork_ready) {            // ...but staged_artwork write may not
//       use staged_artwork;           //    have propagated yet -> stale/freed data
//     }
//
// This is the root cause of Crash #2: EXCVADDR 0x00300010, A15=0x0000abab (heap poison).
// Core 1 was reading staged_artwork.bitmap_data that had already been freed on Core 0.
//
// Fix: artwork_task uses store(true, memory_order_release) which guarantees all writes
// before the store are visible to any thread that subsequently reads with memory_order_acquire.
// apply_pending_artwork uses load(memory_order_acquire), completing the release-acquire pair.
static std::atomic<bool> artwork_ready{false};
static std::atomic<bool> artwork_use_default{false};
static std::atomic<bool> artwork_loading{false};

// Widget to update on UI core. Only touched on Core 1 except for initial assignment.
// Call invalidate_artwork_widget() before destroying any widget to clear this pointer.
static lv_obj_t *artwork_target_widget = nullptr;

// Title/artist copied for the background task (safe from caller lifetime)
static char async_title[128] = { 0 };
static char async_artist[128] = { 0 };

// Debounce state - managed on Core 1 (UI loop) only
static bool artwork_pending = false;
static unsigned long artwork_request_time = 0;


class MediaControls {

public:
  bool get_media_image(const char *title, const char *artist, bitmap_image_t *out_image);
  bool safeSendMediaCommand(AMSRemoteCommand cmd, const char *cmdName);
  void update_album_artwork(const char *title, const char *artist, lv_obj_t *artwork_widget);
  void update_playback_timing(const char *title, const char *artist, bool isPlaying, float remainingSeconds);
  void process_artwork_wifi();
  void apply_pending_artwork();

  // Call this from Core 1 whenever artwork_target_widget is about to be destroyed.
  // With the LV_EVENT_DELETE callback registered by start_async_artwork_update(),
  // this should rarely be needed — LVGL will call artwork_widget_delete_cb()
  // automatically before any object is freed.  Keep calling this from any manual
  // teardown path for belt-and-suspenders safety.
  void invalidate_artwork_widget(lv_obj_t *widget) {
    if (artwork_target_widget == widget) {
      // Remove the event callback so LVGL doesn't fire it after the widget is gone.
      lv_obj_remove_event_cb(widget, artwork_widget_delete_cb);
      artwork_target_widget = nullptr;
    }
  }

  // LVGL LV_EVENT_DELETE callback — registered on artwork_target_widget in
  // start_async_artwork_update(). LVGL calls this BEFORE freeing the object, so
  // artwork_target_widget is nulled while the object memory is still valid.
  // This replaces the unsafe lv_obj_is_valid() probing that caused Crash #3:
  //   lv_obj_is_valid() walks the LVGL object tree and dereferences class_p->vtable.
  //   If the heap has been disturbed by a concurrent HTTPS fetch, class_p can point
  //   to corrupted memory containing 0 as the function pointer. The dispatch to
  //   vtable[-3] = 0 + (-12) = 0xfffffff4 triggers InstrFetchProhibited, the
  //   exception handler tries to call into similarly corrupted state → Double exception.
  static void artwork_widget_delete_cb(lv_event_t *e);

private:
  bool download_and_convert_artwork(const char *image_url, bitmap_image_t *out_image);
  void update_lvgl_image(lv_obj_t *img_obj, bitmap_image_t *bitmap);
  void start_async_artwork_update(const char *title, const char *artist, lv_obj_t *widget);
  void launch_artwork_task();
  void free_artwork();
  void invalidate_img_dsc();  // FIX: nulls img_dsc->data so LVGL never renders freed bitmap
  void mark_artwork_wifi_connected();

  void urlEncode(const char *src, char *dst, size_t dstSize);

  // FIX: Moved from static-local inside update_lvgl_image() to class scope so that
  // free_artwork() and the artwork_use_default path can null img_dsc->data before
  // freeing the underlying bitmap.  Without this, LVGL's render pipeline holds a
  // live pointer into freed heap between the free() call and the next
  // update_lvgl_image() call, which produces a use-after-free if a redraw fires
  // in that window (common during screen transitions).
  lv_img_dsc_t *img_dsc = nullptr;

  // Artwork WiFi lifecycle manager:
  // - preconnect near end of track (UI core)
  // - connect/disconnect + hold window (background core)
  std::atomic<bool> artworkWifiConnectRequested{false};
  std::atomic<bool> artworkWifiConnectedByArtwork{false};
  std::atomic<unsigned long> artworkWifiHoldUntilMs{0};
  std::atomic<unsigned long> artworkWifiLastConnectAttemptMs{0};

  // Track-level preconnect gating (UI core only)
  char preconnectTrackTitle[128] = {0};
  char preconnectTrackArtist[128] = {0};
  bool preconnectIssuedForTrack = false;
};

#endif /* media_controls_h */
