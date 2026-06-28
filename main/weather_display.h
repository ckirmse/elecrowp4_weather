#pragma once

#include <time.h>

#include "lvgl.h"
#include "display.h"
#include "weather_state.h"

class WeatherDisplay {
public:
    WeatherDisplay();
    ~WeatherDisplay() = default;

    void init(Display & display);
    void render(const WeatherState & state);
    void showStartupStatus(const char * msg);
    void showMainScreen();
    void showStatus(const char * msg);
    void showRadioError(const char * msg);

private:
    lv_obj_t * m_startup_screen;
    lv_obj_t * m_startup_label;
    lv_obj_t * m_screen;

    // Right column: outdoor temperature and humidity
    lv_obj_t * m_outdoor_temp_int_label;
    lv_obj_t * m_outdoor_temp_unit_label;
    lv_obj_t * m_outdoor_temp_c_label;
    lv_obj_t * m_outdoor_humidity_label;

    // Left column: date, time, indoor temperature, pressure, indoor humidity
    lv_obj_t * m_indoor_temp_label;
    lv_obj_t * m_indoor_humidity_label;
    lv_obj_t * m_pressure_label;
    lv_obj_t * m_date_label;
    lv_obj_t * m_gas_label;
    lv_obj_t * m_gas_zone_label;

    // Status bar (bottom of screen)
    lv_obj_t * m_status_label;
    lv_obj_t * m_stale_label;

    bool m_is_stale = false;

    lv_style_t m_style_big;
    lv_style_t m_style_value;
    lv_style_t m_style_date;
    lv_style_t m_style_medium;
    lv_style_t m_style_small;
    lv_style_t m_style_dark_bg;

    void checkStaleness(const WeatherState & state);
};
