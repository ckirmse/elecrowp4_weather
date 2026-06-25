#pragma once

#include <time.h>

#include "lvgl.h"
#include "acurite.h"
#include "display.h"

class WeatherDisplay {
public:
    WeatherDisplay();
    ~WeatherDisplay() = default;

    void init(Display & display);
    void update(const AcuriteReading & reading, int rssi_dbm);
    void showStartupStatus(const char * msg);
    void showMainScreen();
    void showWaiting();
    void showRadioError(const char * msg);
    void checkStaleness();

private:
    lv_obj_t * m_startup_screen;
    lv_obj_t * m_startup_label;
    lv_obj_t * m_screen;
    lv_obj_t * m_temp_int_label;    // integer part of temp (big font)
    lv_obj_t * m_temp_unit_label;   // "°F" in medium font
    lv_obj_t * m_temp_c_label;      // "(22.4°C)" in small font
    lv_obj_t * m_humidity_val_label;
    lv_obj_t * m_humidity_unit_label;
    lv_obj_t * m_status_label;
    lv_obj_t * m_update_label;

    time_t m_last_reading_time = 0;
    bool   m_is_stale = false;

    lv_style_t m_style_big;
    lv_style_t m_style_medium;
    lv_style_t m_style_small;
    lv_style_t m_style_dark_bg;
};
