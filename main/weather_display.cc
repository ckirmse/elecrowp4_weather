#include <math.h>
#include <stdio.h>
#include <time.h>
#include <stdlib.h>

#include "weather_display.h"
#include "log_util.h"

static const char * TAG = "WeatherDisplay";

LV_FONT_DECLARE(lv_font_sourcesanspro_bold300);
LV_FONT_DECLARE(lv_font_sourcesanspro_regular24);
LV_FONT_DECLARE(lv_font_sourcesanspro_regular16);

#define COLOR_BG      lv_color_hex(0x1A1A2E)
#define COLOR_PANEL   lv_color_hex(0x16213E)
#define COLOR_ACCENT  lv_color_hex(0x0F3460)
#define COLOR_TEMP    lv_color_hex(0xE94560)
#define COLOR_HUMID   lv_color_hex(0x53D8FB)
#define COLOR_TEXT    lv_color_hex(0xE0E0E0)
#define COLOR_MUTED   lv_color_hex(0x888888)
#define COLOR_STALE_BG lv_color_hex(0xCC0000)

static constexpr time_t STALE_THRESHOLD_S = 600;  // 10 minutes

WeatherDisplay::WeatherDisplay():
    m_startup_screen(nullptr),
    m_startup_label(nullptr),
    m_screen(nullptr),
    m_temp_int_label(nullptr),
    m_temp_unit_label(nullptr),
    m_temp_c_label(nullptr),
    m_humidity_val_label(nullptr),
    m_humidity_unit_label(nullptr),
    m_status_label(nullptr),
    m_update_label(nullptr) {
}

void WeatherDisplay::init(Display & display) {
    lprintf(TAG, "Building LVGL weather UI");

    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();

    // Styles — initialised first so startup screen can use them.
    lv_style_init(&m_style_dark_bg);
    lv_style_set_bg_color(&m_style_dark_bg, COLOR_BG);
    lv_style_set_bg_opa(&m_style_dark_bg, LV_OPA_COVER);
    lv_style_set_border_width(&m_style_dark_bg, 0);
    lv_style_set_pad_all(&m_style_dark_bg, 0);

    lv_style_init(&m_style_big);
    lv_style_set_text_font(&m_style_big, &lv_font_sourcesanspro_bold300);
    lv_style_set_text_color(&m_style_big, COLOR_TEMP);

    lv_style_init(&m_style_medium);
    lv_style_set_text_font(&m_style_medium, &lv_font_sourcesanspro_regular24);
    lv_style_set_text_color(&m_style_medium, COLOR_TEXT);

    lv_style_init(&m_style_small);
    lv_style_set_text_font(&m_style_small, &lv_font_sourcesanspro_regular16);
    lv_style_set_text_color(&m_style_small, COLOR_MUTED);

    // ── Startup screen (shown during WiFi/NTP init) ──────────────────────────
    m_startup_screen = lv_obj_create(NULL);
    lv_obj_add_style(m_startup_screen, &m_style_dark_bg, 0);

    lv_obj_t * startup_title = lv_label_create(m_startup_screen);
    lv_label_set_text(startup_title, "AcuRite Weather Station");
    lv_obj_add_style(startup_title, &m_style_medium, 0);
    lv_obj_set_style_text_color(startup_title, COLOR_MUTED, 0);
    lv_obj_align(startup_title, LV_ALIGN_TOP_MID, 0, 16);

    m_startup_label = lv_label_create(m_startup_screen);
    lv_label_set_text(m_startup_label, "Starting up...");
    lv_obj_add_style(m_startup_label, &m_style_medium, 0);
    lv_obj_align(m_startup_label, LV_ALIGN_CENTER, 0, 0);

    lv_scr_load(m_startup_screen);

    // ── Main weather screen ──────────────────────────────────────────────────
    m_screen = lv_obj_create(NULL);
    lv_obj_add_style(m_screen, &m_style_dark_bg, 0);

    // ── Title ────────────────────────────────────────────────────────────────
    lv_obj_t * title = lv_label_create(m_screen);
    lv_label_set_text(title, "AcuRite Weather Station");
    lv_obj_add_style(title, &m_style_medium, 0);
    lv_obj_set_style_text_color(title, COLOR_MUTED, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    // ── Temperature panel (left half) ────────────────────────────────────────
    lv_obj_t * temp_panel = lv_obj_create(m_screen);
    lv_obj_set_size(temp_panel, 480, 500);
    lv_obj_align(temp_panel, LV_ALIGN_LEFT_MID, 32, 10);
    lv_obj_set_style_bg_color(temp_panel, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(temp_panel, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(temp_panel, 2, 0);
    lv_obj_set_style_radius(temp_panel, 12, 0);
    lv_obj_set_style_pad_all(temp_panel, 0, 0);

    lv_obj_t * temp_title = lv_label_create(temp_panel);
    lv_label_set_text(temp_title, "TEMPERATURE");
    lv_obj_add_style(temp_title, &m_style_small, 0);
    lv_obj_set_style_text_color(temp_title, COLOR_MUTED, 0);
    lv_obj_align(temp_title, LV_ALIGN_TOP_MID, 0, 20);

    m_temp_int_label = lv_label_create(temp_panel);
    lv_label_set_text(m_temp_int_label, "--");
    lv_obj_add_style(m_temp_int_label, &m_style_big, 0);
    lv_obj_align(m_temp_int_label, LV_ALIGN_CENTER, 0, 0);

    m_temp_unit_label = lv_label_create(temp_panel);
    lv_label_set_text(m_temp_unit_label, "\xC2\xB0""F");  // °F in UTF-8
    lv_obj_add_style(m_temp_unit_label, &m_style_medium, 0);
    lv_obj_set_style_text_color(m_temp_unit_label, COLOR_TEMP, 0);
    lv_obj_align(m_temp_unit_label, LV_ALIGN_CENTER, 160, -120);

    m_temp_c_label = lv_label_create(temp_panel);
    lv_label_set_text(m_temp_c_label, "-- \xC2\xB0""C");
    lv_obj_add_style(m_temp_c_label, &m_style_medium, 0);
    lv_obj_set_style_text_color(m_temp_c_label, COLOR_MUTED, 0);
    lv_obj_align(m_temp_c_label, LV_ALIGN_BOTTOM_MID, 0, -16);

    // ── Humidity panel (right half) ──────────────────────────────────────────
    lv_obj_t * hum_panel = lv_obj_create(m_screen);
    lv_obj_set_size(hum_panel, 440, 500);
    lv_obj_align(hum_panel, LV_ALIGN_RIGHT_MID, -32, 10);
    lv_obj_set_style_bg_color(hum_panel, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(hum_panel, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(hum_panel, 2, 0);
    lv_obj_set_style_radius(hum_panel, 12, 0);
    lv_obj_set_style_pad_all(hum_panel, 0, 0);

    lv_obj_t * hum_title = lv_label_create(hum_panel);
    lv_label_set_text(hum_title, "HUMIDITY");
    lv_obj_add_style(hum_title, &m_style_small, 0);
    lv_obj_set_style_text_color(hum_title, COLOR_MUTED, 0);
    lv_obj_align(hum_title, LV_ALIGN_TOP_MID, 0, 20);

    m_humidity_val_label = lv_label_create(hum_panel);
    lv_label_set_text(m_humidity_val_label, "--");
    lv_obj_add_style(m_humidity_val_label, &m_style_big, 0);
    lv_obj_set_style_text_color(m_humidity_val_label, COLOR_HUMID, 0);
    lv_obj_align(m_humidity_val_label, LV_ALIGN_CENTER, 0, 0);

    m_humidity_unit_label = lv_label_create(hum_panel);
    lv_label_set_text(m_humidity_unit_label, "%");
    lv_obj_add_style(m_humidity_unit_label, &m_style_medium, 0);
    lv_obj_set_style_text_color(m_humidity_unit_label, COLOR_HUMID, 0);
    lv_obj_align(m_humidity_unit_label, LV_ALIGN_CENTER, 140, -120);

    // ── Status bar ────────────────────────────────────────────────────────────
    m_status_label = lv_label_create(m_screen);
    lv_label_set_text(m_status_label, "Waiting for sensor...");
    lv_obj_add_style(m_status_label, &m_style_small, 0);
    lv_obj_align(m_status_label, LV_ALIGN_BOTTOM_LEFT, 16, -16);

    m_update_label = lv_label_create(m_screen);
    lv_label_set_text(m_update_label, "");
    lv_obj_add_style(m_update_label, &m_style_small, 0);
    lv_obj_set_style_pad_hor(m_update_label, 8, 0);
    lv_obj_set_style_pad_ver(m_update_label, 4, 0);
    lv_obj_set_style_radius(m_update_label, 4, 0);
    lv_obj_align(m_update_label, LV_ALIGN_BOTTOM_RIGHT, -16, -16);

    lprintf(TAG, "Weather UI ready");
}

void WeatherDisplay::showStartupStatus(const char * msg) {
    lv_label_set_text(m_startup_label, msg);
}

void WeatherDisplay::showMainScreen() {
    lv_scr_load(m_screen);
}

void WeatherDisplay::update(const AcuriteReading & r, int rssi_dbm) {
    char buf[64];

    snprintf(buf, sizeof(buf), "%d", (int)roundf(r.temp_f));
    lv_label_set_text(m_temp_int_label, buf);

    snprintf(buf, sizeof(buf), "%.1f \xC2\xB0""C", r.temp_c);
    lv_label_set_text(m_temp_c_label, buf);

    snprintf(buf, sizeof(buf), "%d", r.humidity);
    lv_label_set_text(m_humidity_val_label, buf);

    snprintf(buf, sizeof(buf), "ID: %04X  Ch: %c  Bat: %s  Signal: %d dBm",
        r.sensor_id, r.channel, r.battery_ok ? "OK" : "LOW", rssi_dbm);
    lv_label_set_text(m_status_label, buf);

    time_t now = time(nullptr);
    struct tm tm_local;
    localtime_r(&now, &tm_local);
    int hour = tm_local.tm_hour % 12;
    if (hour == 0) hour = 12;
    snprintf(buf, sizeof(buf), "data as of %d:%02d%s",
        hour, tm_local.tm_min, tm_local.tm_hour < 12 ? "am" : "pm");
    lv_label_set_text(m_update_label, buf);

    m_last_reading_time = now;
    checkStaleness();
}

void WeatherDisplay::checkStaleness() {
    if (m_last_reading_time == 0) {
        return;
    }
    bool stale = (time(nullptr) - m_last_reading_time) > STALE_THRESHOLD_S;
    if (stale == m_is_stale) {
        return;
    }
    m_is_stale = stale;
    if (stale) {
        lv_obj_set_style_text_color(m_update_label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(m_update_label, COLOR_STALE_BG, 0);
        lv_obj_set_style_bg_opa(m_update_label, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_text_color(m_update_label, COLOR_MUTED, 0);
        lv_obj_set_style_bg_opa(m_update_label, LV_OPA_TRANSP, 0);
    }
}

void WeatherDisplay::showWaiting() {
    lv_label_set_text(m_temp_int_label, "--");
    lv_label_set_text(m_temp_c_label, "--\xC2\xB0""C");
    lv_label_set_text(m_humidity_val_label, "--");
    lv_label_set_text(m_status_label, "Waiting for sensor...");
}

void WeatherDisplay::showRadioError(const char * msg) {
    lv_obj_set_style_bg_color(m_status_label, COLOR_STALE_BG, 0);
    lv_obj_set_style_bg_opa(m_status_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(m_status_label, lv_color_white(), 0);
    lv_label_set_text(m_status_label, msg);
}
