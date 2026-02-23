#ifndef EEZ_LVGL_UI_SCREENS_H
#define EEZ_LVGL_UI_SCREENS_H

#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct _objects_t {
    lv_obj_t *main;
    lv_obj_t *incoming_call;
    lv_obj_t *applications;
    lv_obj_t *alarm_triggered;
    lv_obj_t *settings;
    lv_obj_t *media;
    lv_obj_t *wifi_settings;
    lv_obj_t *wifi_credentials;
    lv_obj_t *weather_settings;
    lv_obj_t *calculator;
    lv_obj_t *alarm;
    lv_obj_t *alarm_sound_picker;
    lv_obj_t *timer_sound_picker;
    lv_obj_t *timer;
    lv_obj_t *stopwatch;
    lv_obj_t *weather_forecast_daily;
    lv_obj_t *weather_forecast_hourly;
    lv_obj_t *notifications;
    lv_obj_t *quick_notification;
    lv_obj_t *calendar;
    lv_obj_t *obj0;
    lv_obj_t *obj0__wifi_signal_strength_image;
    lv_obj_t *obj0__battery_not_charging_image_5;
    lv_obj_t *obj0__battery_charging_image_5;
    lv_obj_t *obj0__battery_percent_5;
    lv_obj_t *obj0__obj0;
    lv_obj_t *obj0__obj1;
    lv_obj_t *obj0__obj2;
    lv_obj_t *obj1;
    lv_obj_t *obj1__wifi_signal_strength_image;
    lv_obj_t *obj1__battery_not_charging_image_5;
    lv_obj_t *obj1__battery_charging_image_5;
    lv_obj_t *obj1__battery_percent_5;
    lv_obj_t *obj1__obj0;
    lv_obj_t *obj1__obj1;
    lv_obj_t *obj1__obj2;
    lv_obj_t *obj2;
    lv_obj_t *obj2__wifi_signal_strength_image;
    lv_obj_t *obj2__battery_not_charging_image_5;
    lv_obj_t *obj2__battery_charging_image_5;
    lv_obj_t *obj2__battery_percent_5;
    lv_obj_t *obj2__obj0;
    lv_obj_t *obj2__obj1;
    lv_obj_t *obj2__obj2;
    lv_obj_t *obj3;
    lv_obj_t *obj3__wifi_signal_strength_image;
    lv_obj_t *obj3__battery_not_charging_image_5;
    lv_obj_t *obj3__battery_charging_image_5;
    lv_obj_t *obj3__battery_percent_5;
    lv_obj_t *obj3__obj0;
    lv_obj_t *obj3__obj1;
    lv_obj_t *obj3__obj2;
    lv_obj_t *obj4;
    lv_obj_t *obj4__wifi_signal_strength_image;
    lv_obj_t *obj4__battery_not_charging_image_5;
    lv_obj_t *obj4__battery_charging_image_5;
    lv_obj_t *obj4__battery_percent_5;
    lv_obj_t *obj4__obj0;
    lv_obj_t *obj4__obj1;
    lv_obj_t *obj4__obj2;
    lv_obj_t *obj5;
    lv_obj_t *obj5__wifi_signal_strength_image;
    lv_obj_t *obj5__battery_not_charging_image_5;
    lv_obj_t *obj5__battery_charging_image_5;
    lv_obj_t *obj5__battery_percent_5;
    lv_obj_t *obj5__obj0;
    lv_obj_t *obj5__obj1;
    lv_obj_t *obj5__obj2;
    lv_obj_t *obj6;
    lv_obj_t *obj6__wifi_signal_strength_image;
    lv_obj_t *obj6__battery_not_charging_image_5;
    lv_obj_t *obj6__battery_charging_image_5;
    lv_obj_t *obj6__battery_percent_5;
    lv_obj_t *obj6__obj0;
    lv_obj_t *obj6__obj1;
    lv_obj_t *obj6__obj2;
    lv_obj_t *obj7;
    lv_obj_t *obj7__wifi_signal_strength_image;
    lv_obj_t *obj7__battery_not_charging_image_5;
    lv_obj_t *obj7__battery_charging_image_5;
    lv_obj_t *obj7__battery_percent_5;
    lv_obj_t *obj7__obj0;
    lv_obj_t *obj7__obj1;
    lv_obj_t *obj7__obj2;
    lv_obj_t *obj8;
    lv_obj_t *obj8__wifi_signal_strength_image;
    lv_obj_t *obj8__battery_not_charging_image_5;
    lv_obj_t *obj8__battery_charging_image_5;
    lv_obj_t *obj8__battery_percent_5;
    lv_obj_t *obj8__obj0;
    lv_obj_t *obj8__obj1;
    lv_obj_t *obj8__obj2;
    lv_obj_t *obj9;
    lv_obj_t *obj9__obj3;
    lv_obj_t *obj9__obj4;
    lv_obj_t *obj10;
    lv_obj_t *obj10__wifi_signal_strength_image;
    lv_obj_t *obj10__battery_not_charging_image_5;
    lv_obj_t *obj10__battery_charging_image_5;
    lv_obj_t *obj10__battery_percent_5;
    lv_obj_t *obj10__obj0;
    lv_obj_t *obj10__obj1;
    lv_obj_t *obj10__obj2;
    lv_obj_t *obj11;
    lv_obj_t *obj11__obj5;
    lv_obj_t *obj11__obj6;
    lv_obj_t *obj12;
    lv_obj_t *obj12__wifi_signal_strength_image;
    lv_obj_t *obj12__battery_not_charging_image_5;
    lv_obj_t *obj12__battery_charging_image_5;
    lv_obj_t *obj12__battery_percent_5;
    lv_obj_t *obj12__obj0;
    lv_obj_t *obj12__obj1;
    lv_obj_t *obj12__obj2;
    lv_obj_t *obj13;
    lv_obj_t *obj13__wifi_signal_strength_image;
    lv_obj_t *obj13__battery_not_charging_image_5;
    lv_obj_t *obj13__battery_charging_image_5;
    lv_obj_t *obj13__battery_percent_5;
    lv_obj_t *obj13__obj0;
    lv_obj_t *obj13__obj1;
    lv_obj_t *obj13__obj2;
    lv_obj_t *obj14;
    lv_obj_t *obj15;
    lv_obj_t *obj16;
    lv_obj_t *obj17;
    lv_obj_t *obj18;
    lv_obj_t *obj19;
    lv_obj_t *obj20;
    lv_obj_t *obj21;
    lv_obj_t *obj22;
    lv_obj_t *obj23;
    lv_obj_t *obj24;
    lv_obj_t *obj25;
    lv_obj_t *obj26;
    lv_obj_t *obj27;
    lv_obj_t *obj28;
    lv_obj_t *obj29;
    lv_obj_t *volume_slider;
    lv_obj_t *obj30;
    lv_obj_t *obj31;
    lv_obj_t *obj32;
    lv_obj_t *obj33;
    lv_obj_t *media_volume_down_button;
    lv_obj_t *obj34;
    lv_obj_t *media_pause_button;
    lv_obj_t *media_play_button;
    lv_obj_t *obj35;
    lv_obj_t *media_volum_up_button;
    lv_obj_t *obj36;
    lv_obj_t *obj37;
    lv_obj_t *obj38;
    lv_obj_t *obj39;
    lv_obj_t *obj40;
    lv_obj_t *obj41;
    lv_obj_t *obj42;
    lv_obj_t *obj43;
    lv_obj_t *weather_location_digit1;
    lv_obj_t *weather_location_digit2;
    lv_obj_t *weather_location_digit3;
    lv_obj_t *weather_location_digit4;
    lv_obj_t *weather_location_digit5;
    lv_obj_t *obj44;
    lv_obj_t *obj45;
    lv_obj_t *calculator_button_matrix;
    lv_obj_t *alarm_hour;
    lv_obj_t *alarm_minute;
    lv_obj_t *alarm_am_pm;
    lv_obj_t *alarm_active;
    lv_obj_t *alarm_tone;
    lv_obj_t *obj46;
    lv_obj_t *obj47;
    lv_obj_t *obj48;
    lv_obj_t *obj49;
    lv_obj_t *obj50;
    lv_obj_t *obj51;
    lv_obj_t *obj52;
    lv_obj_t *obj53;
    lv_obj_t *timer_hour;
    lv_obj_t *timer_minute;
    lv_obj_t *timer_second;
    lv_obj_t *timer_run_button;
    lv_obj_t *timer_stop_button;
    lv_obj_t *time_pause_button;
    lv_obj_t *timer_tone;
    lv_obj_t *obj54;
    lv_obj_t *obj55;
    lv_obj_t *obj56;
    lv_obj_t *obj57;
    lv_obj_t *obj58;
    lv_obj_t *obj59;
    lv_obj_t *notification_positive_button;
    lv_obj_t *notification_negative_button;
    lv_obj_t *obj60;
    lv_obj_t *obj61;
    lv_obj_t *obj62;
    lv_obj_t *obj63;
    lv_obj_t *obj64;
    lv_obj_t *calendar_prev_day;
    lv_obj_t *obj65;
    lv_obj_t *calendar_next_day;
    lv_obj_t *obj66;
    lv_obj_t *main_date_container;
    lv_obj_t *year;
    lv_obj_t *day_of_month;
    lv_obj_t *day_of_week;
    lv_obj_t *time_group;
    lv_obj_t *minute;
    lv_obj_t *hour;
    lv_obj_t *sec_dot_arc;
    lv_obj_t *weather_group;
    lv_obj_t *max_min_temp_clock;
    lv_obj_t *weather_degree_icon;
    lv_obj_t *weather_temperature;
    lv_obj_t *weather_short_summary;
    lv_obj_t *weather_icon_small;
    lv_obj_t *daily_steps_group;
    lv_obj_t *daily_steps;
    lv_obj_t *daily_mission;
    lv_obj_t *daily_steps_pct;
    lv_obj_t *obj67;
    lv_obj_t *daily_step_arc;
    lv_obj_t *obj68;
    lv_obj_t *obj69;
    lv_obj_t *obj70;
    lv_obj_t *obj71;
    lv_obj_t *obj72;
    lv_obj_t *obj73;
    lv_obj_t *obj74;
    lv_obj_t *obj75;
    lv_obj_t *obj76;
    lv_obj_t *obj77;
    lv_obj_t *obj78;
    lv_obj_t *obj79;
    lv_obj_t *obj80;
    lv_obj_t *obj81;
    lv_obj_t *obj82;
    lv_obj_t *obj83;
    lv_obj_t *alarm_icon;
    lv_obj_t *snooze;
    lv_obj_t *obj84;
    lv_obj_t *snooze_confirmation;
    lv_obj_t *snooze_icon;
    lv_obj_t *obj85;
    lv_obj_t *obj86;
    lv_obj_t *obj87;
    lv_obj_t *obj88;
    lv_obj_t *obj89;
    lv_obj_t *obj90;
    lv_obj_t *daily_steps_goal_slider;
    lv_obj_t *obj91;
    lv_obj_t *brightness_slider;
    lv_obj_t *obj92;
    lv_obj_t *settings_restart_button;
    lv_obj_t *obj93;
    lv_obj_t *obj94;
    lv_obj_t *settings_black_screen;
    lv_obj_t *obj95;
    lv_obj_t *media_app_image;
    lv_obj_t *media_app_name;
    lv_obj_t *media_image;
    lv_obj_t *obj96;
    lv_obj_t *obj97;
    lv_obj_t *obj98;
    lv_obj_t *obj99;
    lv_obj_t *media_prev_track_button;
    lv_obj_t *obj100;
    lv_obj_t *media_next_track_button;
    lv_obj_t *obj101;
    lv_obj_t *obj102;
    lv_obj_t *obj103;
    lv_obj_t *obj104;
    lv_obj_t *obj105;
    lv_obj_t *obj106;
    lv_obj_t *wifi_available_network_list;
    lv_obj_t *obj107;
    lv_obj_t *obj108;
    lv_obj_t *obj109;
    lv_obj_t *wifi_password_field;
    lv_obj_t *obj110;
    lv_obj_t *obj111;
    lv_obj_t *obj112;
    lv_obj_t *obj113;
    lv_obj_t *obj114;
    lv_obj_t *obj115;
    lv_obj_t *obj116;
    lv_obj_t *obj117;
    lv_obj_t *weather_settings_spinner;
    lv_obj_t *obj118;
    lv_obj_t *calculator_input;
    lv_obj_t *calculator_history;
    lv_obj_t *obj119;
    lv_obj_t *obj120;
    lv_obj_t *obj121;
    lv_obj_t *obj122;
    lv_obj_t *obj123;
    lv_obj_t *obj124;
    lv_obj_t *obj125;
    lv_obj_t *obj126;
    lv_obj_t *obj127;
    lv_obj_t *obj128;
    lv_obj_t *obj129;
    lv_obj_t *obj130;
    lv_obj_t *obj131;
    lv_obj_t *obj132;
    lv_obj_t *obj133;
    lv_obj_t *obj134;
    lv_obj_t *obj135;
    lv_obj_t *obj136;
    lv_obj_t *obj137;
    lv_obj_t *obj138;
    lv_obj_t *obj139;
    lv_obj_t *icon_forecast_0;
    lv_obj_t *obj140;
    lv_obj_t *icon_forecast_1;
    lv_obj_t *obj141;
    lv_obj_t *icon_forecast_2;
    lv_obj_t *obj142;
    lv_obj_t *icon_forecast_3;
    lv_obj_t *obj143;
    lv_obj_t *icon_forecast_4;
    lv_obj_t *obj144;
    lv_obj_t *icon_forecast_5;
    lv_obj_t *obj145;
    lv_obj_t *icon_forecast_6;
    lv_obj_t *obj146;
    lv_obj_t *icon_forecast_7;
    lv_obj_t *obj147;
    lv_obj_t *icon_forecast_8;
    lv_obj_t *obj148;
    lv_obj_t *icon_forecast_9;
    lv_obj_t *daily_forecast_blank;
    lv_obj_t *weather_forecast_daily_gear;
    lv_obj_t *obj149;
    lv_obj_t *obj150;
    lv_obj_t *obj151;
    lv_obj_t *obj152;
    lv_obj_t *obj153;
    lv_obj_t *obj154;
    lv_obj_t *obj155;
    lv_obj_t *obj156;
    lv_obj_t *obj157;
    lv_obj_t *obj158;
    lv_obj_t *obj159;
    lv_obj_t *obj160;
    lv_obj_t *obj161;
    lv_obj_t *obj162;
    lv_obj_t *obj163;
    lv_obj_t *obj164;
    lv_obj_t *obj165;
    lv_obj_t *obj166;
    lv_obj_t *obj167;
    lv_obj_t *obj168;
    lv_obj_t *obj169;
    lv_obj_t *obj170;
    lv_obj_t *weather_icon_large;
    lv_obj_t *obj171;
    lv_obj_t *humidity_icon_2;
    lv_obj_t *obj172;
    lv_obj_t *obj173;
    lv_obj_t *wind_icon_2;
    lv_obj_t *obj174;
    lv_obj_t *obj175;
    lv_obj_t *hour_icon_0;
    lv_obj_t *obj176;
    lv_obj_t *obj177;
    lv_obj_t *hour_icon_1;
    lv_obj_t *obj178;
    lv_obj_t *obj179;
    lv_obj_t *hour_icon_2;
    lv_obj_t *obj180;
    lv_obj_t *obj181;
    lv_obj_t *hour_icon_3;
    lv_obj_t *obj182;
    lv_obj_t *obj183;
    lv_obj_t *hour_icon_4;
    lv_obj_t *obj184;
    lv_obj_t *obj185;
    lv_obj_t *hour_icon_5;
    lv_obj_t *obj186;
    lv_obj_t *obj187;
    lv_obj_t *hour_icon_6;
    lv_obj_t *obj188;
    lv_obj_t *obj189;
    lv_obj_t *hour_icon_7;
    lv_obj_t *weather_forecast_hourly_gear;
    lv_obj_t *obj190;
    lv_obj_t *obj191;
    lv_obj_t *obj192;
    lv_obj_t *obj193;
    lv_obj_t *obj194;
    lv_obj_t *obj195;
    lv_obj_t *obj196;
    lv_obj_t *obj197;
    lv_obj_t *notification_icon_widget;
    lv_obj_t *obj198;
    lv_obj_t *obj199;
    lv_obj_t *obj200;
    lv_obj_t *obj201;
    lv_obj_t *obj202;
    lv_obj_t *obj203;
    lv_obj_t *obj204;
    lv_obj_t *obj205;
    lv_obj_t *quick_notification_icon_widget;
    lv_obj_t *obj206;
    lv_obj_t *obj207;
    lv_obj_t *obj208;
    lv_obj_t *obj209;
    lv_obj_t *obj210;
    lv_obj_t *obj211;
    lv_obj_t *calendar_prev_day_1;
    lv_obj_t *calendar_next_day_1;
    lv_obj_t *obj212;
    lv_obj_t *obj213;
    lv_obj_t *calender_events_container;
    lv_obj_t *obj214;
    lv_obj_t *obj215;
} objects_t;

extern objects_t objects;

enum ScreensEnum {
    SCREEN_ID_MAIN = 1,
    SCREEN_ID_INCOMING_CALL = 2,
    SCREEN_ID_APPLICATIONS = 3,
    SCREEN_ID_ALARM_TRIGGERED = 4,
    SCREEN_ID_SETTINGS = 5,
    SCREEN_ID_MEDIA = 6,
    SCREEN_ID_WIFI_SETTINGS = 7,
    SCREEN_ID_WIFI_CREDENTIALS = 8,
    SCREEN_ID_WEATHER_SETTINGS = 9,
    SCREEN_ID_CALCULATOR = 10,
    SCREEN_ID_ALARM = 11,
    SCREEN_ID_ALARM_SOUND_PICKER = 12,
    SCREEN_ID_TIMER_SOUND_PICKER = 13,
    SCREEN_ID_TIMER = 14,
    SCREEN_ID_STOPWATCH = 15,
    SCREEN_ID_WEATHER_FORECAST_DAILY = 16,
    SCREEN_ID_WEATHER_FORECAST_HOURLY = 17,
    SCREEN_ID_NOTIFICATIONS = 18,
    SCREEN_ID_QUICK_NOTIFICATION = 19,
    SCREEN_ID_CALENDAR = 20,
};

void create_screen_main();
void delete_screen_main();
void tick_screen_main();

void create_screen_incoming_call();
void delete_screen_incoming_call();
void tick_screen_incoming_call();

void create_screen_applications();
void delete_screen_applications();
void tick_screen_applications();

void create_screen_alarm_triggered();
void delete_screen_alarm_triggered();
void tick_screen_alarm_triggered();

void create_screen_settings();
void delete_screen_settings();
void tick_screen_settings();

void create_screen_media();
void delete_screen_media();
void tick_screen_media();

void create_screen_wifi_settings();
void delete_screen_wifi_settings();
void tick_screen_wifi_settings();

void create_screen_wifi_credentials();
void delete_screen_wifi_credentials();
void tick_screen_wifi_credentials();

void create_screen_weather_settings();
void delete_screen_weather_settings();
void tick_screen_weather_settings();

void create_screen_calculator();
void delete_screen_calculator();
void tick_screen_calculator();

void create_screen_alarm();
void delete_screen_alarm();
void tick_screen_alarm();

void create_screen_alarm_sound_picker();
void delete_screen_alarm_sound_picker();
void tick_screen_alarm_sound_picker();

void create_screen_timer_sound_picker();
void delete_screen_timer_sound_picker();
void tick_screen_timer_sound_picker();

void create_screen_timer();
void delete_screen_timer();
void tick_screen_timer();

void create_screen_stopwatch();
void delete_screen_stopwatch();
void tick_screen_stopwatch();

void create_screen_weather_forecast_daily();
void delete_screen_weather_forecast_daily();
void tick_screen_weather_forecast_daily();

void create_screen_weather_forecast_hourly();
void delete_screen_weather_forecast_hourly();
void tick_screen_weather_forecast_hourly();

void create_screen_notifications();
void delete_screen_notifications();
void tick_screen_notifications();

void create_screen_quick_notification();
void delete_screen_quick_notification();
void tick_screen_quick_notification();

void create_screen_calendar();
void delete_screen_calendar();
void tick_screen_calendar();

void create_user_widget_dock(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_dock(void *flowState, int startWidgetIndex);

void create_user_widget_two_dots_bottom(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_two_dots_bottom(void *flowState, int startWidgetIndex);

void create_user_widget_two_dots_top(lv_obj_t *parent_obj, void *flowState, int startWidgetIndex);
void tick_user_widget_two_dots_top(void *flowState, int startWidgetIndex);

void create_screen_by_id(enum ScreensEnum screenId);
void delete_screen_by_id(enum ScreensEnum screenId);
void tick_screen_by_id(enum ScreensEnum screenId);
void tick_screen(int screen_index);

void create_screens();


#ifdef __cplusplus
}
#endif

#endif /*EEZ_LVGL_UI_SCREENS_H*/