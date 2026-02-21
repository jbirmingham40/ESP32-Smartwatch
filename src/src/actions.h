#ifndef EEZ_LVGL_UI_EVENTS_H
#define EEZ_LVGL_UI_EVENTS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

extern void action_calculator_button_click(lv_event_t * e);
extern void action_stop_alarm_sound(lv_event_t * e);
extern void action_snooze_alarm(lv_event_t * e);
extern void action_play_timer_sound(lv_event_t * e);
extern void action_stop_timer_sound(lv_event_t * e);
extern void action_activate_alarm(lv_event_t * e);
extern void action_check_alarm_native(lv_event_t * e);

enum {
    ACTION_CREATE_WEATHER_LOCATION_PROPERTY_SHOW_INVALID_ZIPCODE,
};
extern void action_create_weather_location(lv_event_t * e);

extern void action_delete_weather_location(lv_event_t * e);
extern void action_load_weather_locations(lv_event_t * e);
extern void action_update_alarm_timer_settings(lv_event_t * e);
extern void action_update_weather_location(lv_event_t * e);
extern void action_load_notifications(lv_event_t * e);
extern void action_next_notification(lv_event_t * e);
extern void action_prev_notification(lv_event_t * e);
extern void action_dismiss_notification(lv_event_t * e);
extern void action_accept_notification(lv_event_t * e);
extern void action_accept_call(lv_event_t * e);
extern void action_decline_call(lv_event_t * e);
extern void action_forget_wifi_network(lv_event_t * e);
extern void action_join_wifi_network(lv_event_t * e);
extern void action_load_available_wifi_networks(lv_event_t * e);
extern void action_dismiss_quick_notification(lv_event_t * e);
extern void action_wifi_screen_unloaded(lv_event_t * e);
extern void action_wifi_screen_loaded(lv_event_t * e);
extern void action_settings_screen_load(lv_event_t * e);
extern void action_restart_smartwatch(lv_event_t * e);
extern void action_play_volume_change_sound(lv_event_t * e);
extern void action_settings_screen_unload(lv_event_t * e);
extern void action_media_play(lv_event_t * e);
extern void action_media_pause(lv_event_t * e);
extern void action_media_toggle_play_pause(lv_event_t * e);
extern void action_media_next_track(lv_event_t * e);
extern void action_media_volume_up(lv_event_t * e);
extern void action_media_volume_down(lv_event_t * e);
extern void action_refresh_media_info(lv_event_t * e);
extern void action_media_previous_track(lv_event_t * e);
extern void action_update_next_alarm_string(lv_event_t * e);
extern void action_play_alarm_sound(lv_event_t * e);
extern void action_calendar_prev_day(lv_event_t * e);
extern void action_calendar_next_day(lv_event_t * e);
extern void action_calendar_today(lv_event_t * e);
extern void action_calendar_picker_update_days(lv_event_t * e);
extern void action_calendar_refresh(lv_event_t * e);
extern void action_calendar_date_changed(lv_event_t * e);
extern void action_return_to_call(lv_event_t * e);
extern void action_update_call_duration(lv_event_t * e);


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_EVENTS_H*/