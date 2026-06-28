#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <stdlib.h>

#include "esp_timer.h"

#include "weather_display.h"
#include "log_util.h"

// Uncomment to force a worst-case date for testing the adaptive abbreviation.
// #define FORCE_TEST_DATE

static const char * TAG = "WeatherDisplay";

LV_FONT_DECLARE(lv_font_sourcesanspro_bold300);
LV_FONT_DECLARE(lv_font_sourcesanspro_bold72);
LV_FONT_DECLARE(lv_font_sourcesanspro_bold36);
LV_FONT_DECLARE(lv_font_sourcesanspro_regular24);
LV_FONT_DECLARE(lv_font_sourcesanspro_regular16);

#define COLOR_BG        lv_color_hex(0x1A1A2E)
#define COLOR_PANEL     lv_color_hex(0x16213E)
#define COLOR_ACCENT    lv_color_hex(0x0F3460)
#define COLOR_TEMP      lv_color_hex(0xE94560)
#define COLOR_HUMID     lv_color_hex(0x53D8FB)
#define COLOR_PRESSURE  lv_color_hex(0x6BCB77)
#define COLOR_INDOOR    lv_color_hex(0xF4A261)
#define COLOR_TEXT      lv_color_hex(0xE0E0E0)
#define COLOR_MUTED     lv_color_hex(0x888888)
#define COLOR_STALE_BG  lv_color_hex(0xCC0000)

static constexpr time_t STALE_THRESHOLD_SEC = 180;

struct IaqZone {
    const char * label;
    lv_color_t color;
};

static constexpr float GAS_RESISTANCE_CLEAN_OHM = 60000.0f;
static constexpr float GAS_RESISTANCE_DIRTY_OHM = 500.0f;

static IaqZone iaq_zone_for(int iaq) {
    if (iaq <= 50)  return { "EXCELLENT",     lv_color_hex(0x00CC00) };
    if (iaq <= 100) return { "GOOD",           lv_color_hex(0x88CC00) };
    if (iaq <= 150) return { "FAIR",           lv_color_hex(0xCCCC00) };
    if (iaq <= 200) return { "POOR",           lv_color_hex(0xFF8000) };
    if (iaq <= 250) return { "VERY POOR",      lv_color_hex(0xFF6060) };
    if (iaq <= 350) return { "SEVERELY POOR",  lv_color_hex(0xCC40CC) };
    return              { "HAZARDOUS",      lv_color_hex(0xFF0000) };
}

static int gas_resistance_to_iaq(float resistance_ohm) {
    float log_clean = log10f(GAS_RESISTANCE_CLEAN_OHM);
    float log_dirty = log10f(GAS_RESISTANCE_DIRTY_OHM);
    float iaq = (log_clean - log10f(resistance_ohm)) / (log_clean - log_dirty) * 500.0f;
    return (int)roundf(fmaxf(0.0f, fminf(500.0f, iaq)));
}

// Layout constants
static constexpr int MARGIN = 12;
static constexpr int GAP = 10;

static constexpr int LEFT_X = MARGIN;
static constexpr int LEFT_W = 510;
static constexpr int RIGHT_X = LEFT_X + LEFT_W + GAP;
static constexpr int RIGHT_W = 1024 - RIGHT_X - MARGIN;  // 480

static constexpr int PANEL_TOP = MARGIN;
static constexpr int PANEL_BOTTOM = 600 - 28;  // leave 28px for status bar
static constexpr int PANEL_H = PANEL_BOTTOM - PANEL_TOP;  // 560

static constexpr int RIGHT_COUNT = 5;
static constexpr int RIGHT_GAP = 8;
static constexpr int RIGHT_PANEL_H = (PANEL_H - (RIGHT_COUNT - 1) * RIGHT_GAP) / RIGHT_COUNT;  // 104

static constexpr int OUTDOOR_HUM_H = RIGHT_PANEL_H;
// Derived so the outdoor humidity panel has the same Y as the indoor humidity panel:
// PANEL_TOP + OUTDOOR_TEMP_H + RIGHT_GAP == PANEL_TOP + 4 * (RIGHT_PANEL_H + RIGHT_GAP)
static constexpr int OUTDOOR_TEMP_H = 4 * RIGHT_PANEL_H + 3 * RIGHT_GAP;

WeatherDisplay::WeatherDisplay():
    m_screen(nullptr),
    m_outdoor_temp_int_label(nullptr),
    m_outdoor_temp_unit_label(nullptr),
    m_outdoor_temp_c_label(nullptr),
    m_outdoor_humidity_label(nullptr),
    m_indoor_temp_label(nullptr),
    m_indoor_humidity_label(nullptr),
    m_pressure_label(nullptr),
    m_date_label(nullptr),
    m_gas_label(nullptr),
    m_gas_zone_label(nullptr),
    m_status_label(nullptr),
    m_stale_label(nullptr) {
}

static lv_obj_t * make_panel(lv_obj_t * parent, int x, int y, int w, int h) {
    lv_obj_t * panel = lv_obj_create(parent);
    lv_obj_set_pos(panel, x, y);
    lv_obj_set_size(panel, w, h);
    lv_obj_set_style_bg_color(panel, COLOR_PANEL, 0);
    lv_obj_set_style_border_color(panel, COLOR_ACCENT, 0);
    lv_obj_set_style_border_width(panel, 4, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    lv_obj_set_style_pad_all(panel, 0, 0);
    lv_obj_clear_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    return panel;
}

// Transparent flex-row container, LV_SIZE_CONTENT, centered in panel at y+10.
// cross_align controls vertical alignment of children (START=superscript, CENTER, END=subscript).
static lv_obj_t * make_flex_row(lv_obj_t * panel, lv_flex_align_t cross_align) {
    lv_obj_t * row = lv_obj_create(panel);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_style_pad_column(row, 4, 0);
    lv_obj_set_layout(row, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, cross_align, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_CENTER, 0, 10);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

void WeatherDisplay::init(Display & display) {
    lprintf(TAG, "Building LVGL weather UI");

    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();

    // Styles
    lv_style_init(&m_style_dark_bg);
    lv_style_set_bg_color(&m_style_dark_bg, COLOR_BG);
    lv_style_set_bg_opa(&m_style_dark_bg, LV_OPA_COVER);
    lv_style_set_border_width(&m_style_dark_bg, 0);
    lv_style_set_pad_all(&m_style_dark_bg, 0);

    lv_style_init(&m_style_big);
    lv_style_set_text_font(&m_style_big, &lv_font_sourcesanspro_bold300);
    lv_style_set_text_color(&m_style_big, COLOR_PRESSURE);

    lv_style_init(&m_style_value);
    lv_style_set_text_font(&m_style_value, &lv_font_sourcesanspro_bold72);
    lv_style_set_text_color(&m_style_value, COLOR_TEXT);

    lv_style_init(&m_style_date);
    lv_style_set_text_font(&m_style_date, &lv_font_sourcesanspro_bold36);
    lv_style_set_text_color(&m_style_date, COLOR_TEXT);

    lv_style_init(&m_style_medium);
    lv_style_set_text_font(&m_style_medium, &lv_font_sourcesanspro_regular24);
    lv_style_set_text_color(&m_style_medium, COLOR_TEXT);

    lv_style_init(&m_style_small);
    lv_style_set_text_font(&m_style_small, &lv_font_sourcesanspro_regular16);
    lv_style_set_text_color(&m_style_small, COLOR_MUTED);

    // ── Main weather screen ─────────────────────────────────────────────────────
    m_screen = lv_obj_create(NULL);
    lv_obj_add_style(m_screen, &m_style_dark_bg, 0);
    lv_obj_clear_flag(m_screen, LV_OBJ_FLAG_SCROLLABLE);

    // ── Left column: 5 stacked panels ──────────────────────────────────────────
    const int left_ys[RIGHT_COUNT] = {
        PANEL_TOP,
        PANEL_TOP + (RIGHT_PANEL_H + RIGHT_GAP),
        PANEL_TOP + 2 * (RIGHT_PANEL_H + RIGHT_GAP),
        PANEL_TOP + 3 * (RIGHT_PANEL_H + RIGHT_GAP),
        PANEL_TOP + 4 * (RIGHT_PANEL_H + RIGHT_GAP),
    };

    // Date / Time
    {
        lv_obj_t * panel = make_panel(m_screen, LEFT_X, left_ys[0], LEFT_W, RIGHT_PANEL_H);
        lv_obj_t * title = lv_label_create(panel);
        lv_label_set_text(title, "DATE / TIME");
        lv_obj_add_style(title, &m_style_small, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
        m_date_label = lv_label_create(panel);
        lv_label_set_text(m_date_label, "--");
        lv_obj_add_style(m_date_label, &m_style_date, 0);
        lv_obj_align(m_date_label, LV_ALIGN_CENTER, 0, 10);
    }

    // Indoor temperature
    {
        lv_obj_t * panel = make_panel(m_screen, LEFT_X, left_ys[1], LEFT_W, RIGHT_PANEL_H);
        lv_obj_t * title = lv_label_create(panel);
        lv_label_set_text(title, "INDOOR TEMPERATURE");
        lv_obj_add_style(title, &m_style_small, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t * row = make_flex_row(panel, LV_FLEX_ALIGN_START);
        m_indoor_temp_label = lv_label_create(row);
        lv_label_set_text(m_indoor_temp_label, "--");
        lv_obj_add_style(m_indoor_temp_label, &m_style_value, 0);
        lv_obj_set_style_text_color(m_indoor_temp_label, COLOR_PRESSURE, 0);
        lv_obj_t * in_temp_unit = lv_label_create(row);
        lv_label_set_text(in_temp_unit, "\xC2\xB0""F");
        lv_obj_add_style(in_temp_unit, &m_style_medium, 0);
        lv_obj_set_style_text_color(in_temp_unit, COLOR_PRESSURE, 0);
        lv_obj_set_style_translate_y(in_temp_unit, 10, 0);
    }

    // Pressure
    {
        lv_obj_t * panel = make_panel(m_screen, LEFT_X, left_ys[2], LEFT_W, RIGHT_PANEL_H);
        lv_obj_t * title = lv_label_create(panel);
        lv_label_set_text(title, "PRESSURE");
        lv_obj_add_style(title, &m_style_small, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t * row = make_flex_row(panel, LV_FLEX_ALIGN_END);
        m_pressure_label = lv_label_create(row);
        lv_label_set_text(m_pressure_label, "--");
        lv_obj_add_style(m_pressure_label, &m_style_value, 0);
        lv_obj_set_style_text_color(m_pressure_label, COLOR_INDOOR, 0);
        lv_obj_t * pres_unit = lv_label_create(row);
        lv_label_set_text(pres_unit, "hPa");
        lv_obj_add_style(pres_unit, &m_style_medium, 0);
        lv_obj_set_style_text_color(pres_unit, COLOR_INDOOR, 0);
        lv_obj_set_style_translate_y(pres_unit, -12, 0);
    }

    // Indoor humidity
    {
        lv_obj_t * panel = make_panel(m_screen, LEFT_X, left_ys[4], LEFT_W, RIGHT_PANEL_H);
        lv_obj_t * title = lv_label_create(panel);
        lv_label_set_text(title, "INDOOR HUMIDITY");
        lv_obj_add_style(title, &m_style_small, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t * row = make_flex_row(panel, LV_FLEX_ALIGN_END);
        m_indoor_humidity_label = lv_label_create(row);
        lv_label_set_text(m_indoor_humidity_label, "--");
        lv_obj_add_style(m_indoor_humidity_label, &m_style_value, 0);
        lv_obj_set_style_text_color(m_indoor_humidity_label, COLOR_HUMID, 0);
        lv_obj_t * in_hum_unit = lv_label_create(row);
        lv_label_set_text(in_hum_unit, "%");
        lv_obj_add_style(in_hum_unit, &m_style_medium, 0);
        lv_obj_set_style_text_color(in_hum_unit, COLOR_HUMID, 0);
        lv_obj_set_style_translate_y(in_hum_unit, -12, 0);
    }

    // Air quality (IAQ)
    {
        lv_obj_t * panel = make_panel(m_screen, LEFT_X, left_ys[3], LEFT_W, RIGHT_PANEL_H);
        lv_obj_t * title = lv_label_create(panel);
        lv_label_set_text(title, "AIR QUALITY");
        lv_obj_add_style(title, &m_style_small, 0);
        lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
        lv_obj_t * row = make_flex_row(panel, LV_FLEX_ALIGN_END);
        m_gas_label = lv_label_create(row);
        lv_label_set_text(m_gas_label, "--");
        lv_obj_add_style(m_gas_label, &m_style_value, 0);
        m_gas_zone_label = lv_label_create(row);
        lv_label_set_text(m_gas_zone_label, "");
        lv_obj_add_style(m_gas_zone_label, &m_style_medium, 0);
        lv_obj_set_style_translate_y(m_gas_zone_label, -12, 0);
    }

    // ── Outdoor temperature panel (right, large) ────────────────────────────────
    lv_obj_t * temp_panel = make_panel(m_screen,
        RIGHT_X, PANEL_TOP, RIGHT_W, OUTDOOR_TEMP_H);

    lv_obj_t * temp_title = lv_label_create(temp_panel);
    lv_label_set_text(temp_title, "OUTDOOR TEMPERATURE");
    lv_obj_add_style(temp_title, &m_style_small, 0);
    lv_obj_align(temp_title, LV_ALIGN_TOP_MID, 0, 12);

    m_outdoor_temp_int_label = lv_label_create(temp_panel);
    lv_label_set_text(m_outdoor_temp_int_label, "--");
    lv_obj_add_style(m_outdoor_temp_int_label, &m_style_big, 0);
    lv_obj_align(m_outdoor_temp_int_label, LV_ALIGN_CENTER, 0, 5);

    m_outdoor_temp_unit_label = lv_label_create(temp_panel);
    lv_label_set_text(m_outdoor_temp_unit_label, "\xC2\xB0""F");
    lv_obj_add_style(m_outdoor_temp_unit_label, &m_style_medium, 0);
    lv_obj_set_style_text_color(m_outdoor_temp_unit_label, COLOR_PRESSURE, 0);
    lv_obj_align(m_outdoor_temp_unit_label, LV_ALIGN_CENTER, 166, -80);

    m_outdoor_temp_c_label = lv_label_create(temp_panel);
    lv_label_set_text(m_outdoor_temp_c_label, "--\xC2\xB0""C");
    lv_obj_add_style(m_outdoor_temp_c_label, &m_style_medium, 0);
    lv_obj_set_style_text_color(m_outdoor_temp_c_label, COLOR_MUTED, 0);
    lv_obj_align(m_outdoor_temp_c_label, LV_ALIGN_BOTTOM_MID, 0, -14);

    // ── Outdoor humidity panel (right, small) ───────────────────────────────────
    int hum_y = PANEL_TOP + OUTDOOR_TEMP_H + RIGHT_GAP;
    lv_obj_t * hum_panel = make_panel(m_screen,
        RIGHT_X, hum_y, RIGHT_W, OUTDOOR_HUM_H);

    lv_obj_t * hum_title = lv_label_create(hum_panel);
    lv_label_set_text(hum_title, "OUTDOOR HUMIDITY");
    lv_obj_add_style(hum_title, &m_style_small, 0);
    lv_obj_align(hum_title, LV_ALIGN_TOP_MID, 0, 10);

    {
        lv_obj_t * row = make_flex_row(hum_panel, LV_FLEX_ALIGN_END);
        m_outdoor_humidity_label = lv_label_create(row);
        lv_label_set_text(m_outdoor_humidity_label, "--");
        lv_obj_add_style(m_outdoor_humidity_label, &m_style_value, 0);
        lv_obj_set_style_text_color(m_outdoor_humidity_label, COLOR_HUMID, 0);
        lv_obj_t * out_hum_unit = lv_label_create(row);
        lv_label_set_text(out_hum_unit, "%");
        lv_obj_add_style(out_hum_unit, &m_style_medium, 0);
        lv_obj_set_style_text_color(out_hum_unit, COLOR_HUMID, 0);
        lv_obj_set_style_translate_y(out_hum_unit, -12, 0);
    }

    // ── Status bar (bottom of screen) ───────────────────────────────────────────
    m_status_label = lv_label_create(m_screen);
    lv_label_set_text(m_status_label, "Waiting for sensor...");
    lv_obj_add_style(m_status_label, &m_style_small, 0);
    lv_obj_align(m_status_label, LV_ALIGN_BOTTOM_LEFT, 16, -8);

    m_stale_label = lv_label_create(m_screen);
    lv_label_set_text(m_stale_label, "");
    lv_obj_add_style(m_stale_label, &m_style_small, 0);
    lv_obj_set_style_pad_hor(m_stale_label, 8, 0);
    lv_obj_set_style_pad_ver(m_stale_label, 4, 0);
    lv_obj_set_style_radius(m_stale_label, 4, 0);
    lv_obj_align(m_stale_label, LV_ALIGN_BOTTOM_RIGHT, -16, -8);

    lv_scr_load(m_screen);
    lprintf(TAG, "Weather UI ready");
}

void WeatherDisplay::showStatus(const char * msg) {
    lv_label_set_text(m_status_label, msg);
}

void WeatherDisplay::showRadioError(const char * msg) {
    lv_obj_set_style_bg_color(m_status_label, COLOR_STALE_BG, 0);
    lv_obj_set_style_bg_opa(m_status_label, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(m_status_label, lv_color_white(), 0);
    lv_label_set_text(m_status_label, msg);
}

static constexpr int DATE_MAX_W = LEFT_W - 24;

static void build_date_string(char * buf, size_t bufsz,
                              const lv_font_t * font, const struct tm * t) {
    int hour = t->tm_hour % 12;
    if (hour == 0) { hour = 12; }
    const char * ampm = t->tm_hour < 12 ? "am" : "pm";

    static const char * const day_fmts[] = { "%A", "%A", "%a" };
    static const char * const mon_fmts[] = { "%B", "%b", "%b" };

    for (int i = 0; i < 3; i++) {
        char date_part[32];
        char fmt[16];
        snprintf(fmt, sizeof(fmt), "%s, %s", day_fmts[i], mon_fmts[i]);
        strftime(date_part, sizeof(date_part), fmt, t);
        snprintf(buf, bufsz, "%s %d \xC2\xB7 %d:%02d %s",
                 date_part, t->tm_mday, hour, t->tm_min, ampm);
        lv_point_t sz;
        lv_text_get_size(&sz, buf, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
        if (sz.x <= DATE_MAX_W || i == 2) {
            break;
        }
    }
}

void WeatherDisplay::render(const WeatherState & state) {
    char buf[64];

    // Outdoor temp
    if (state.outdoor_valid) {
        snprintf(buf, sizeof(buf), "%d", (int)roundf(state.outdoor_temp_f));
        lv_label_set_text(m_outdoor_temp_int_label, buf);

        snprintf(buf, sizeof(buf), "%.1f\xC2\xB0""C", state.outdoor_temp_c);
        lv_label_set_text(m_outdoor_temp_c_label, buf);

        snprintf(buf, sizeof(buf), "%d", state.outdoor_humidity);
        lv_label_set_text(m_outdoor_humidity_label, buf);

        snprintf(buf, sizeof(buf), "ID: %04X  Ch: %c  Bat: %s  Signal: %d dBm",
            state.outdoor_sensor_id, state.outdoor_channel,
            state.outdoor_battery_ok ? "OK" : "LOW", state.outdoor_rssi_dbm);
        lv_label_set_text(m_status_label, buf);
        lv_obj_set_style_bg_opa(m_status_label, LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(m_status_label, COLOR_MUTED, 0);
    } else {
        lv_label_set_text(m_outdoor_temp_int_label, "--");
        lv_label_set_text(m_outdoor_temp_c_label, "--\xC2\xB0""C");
        lv_label_set_text(m_outdoor_humidity_label, "--");
    }

    // Indoor
    if (state.indoor_valid) {
        snprintf(buf, sizeof(buf), "%d", (int)roundf(state.indoor_temp_f));
        lv_label_set_text(m_indoor_temp_label, buf);

        snprintf(buf, sizeof(buf), "%d", (int)roundf(state.indoor_humidity_pct));
        lv_label_set_text(m_indoor_humidity_label, buf);

        snprintf(buf, sizeof(buf), "%d", (int)roundf(state.indoor_pressure_hpa));
        lv_label_set_text(m_pressure_label, buf);

        int iaq = gas_resistance_to_iaq(state.indoor_gas_resistance_ohm);
        IaqZone zone = iaq_zone_for(iaq);
        snprintf(buf, sizeof(buf), "%d", iaq);
        lv_label_set_text(m_gas_label, buf);
        lv_label_set_text(m_gas_zone_label, zone.label);
        lv_obj_set_style_text_color(m_gas_label, zone.color, 0);
        lv_obj_set_style_text_color(m_gas_zone_label, zone.color, 0);
    } else {
        lv_label_set_text(m_indoor_temp_label, "--");
        lv_label_set_text(m_indoor_humidity_label, "--");
        lv_label_set_text(m_pressure_label, "--");
        lv_label_set_text(m_gas_label, "--");
        lv_label_set_text(m_gas_zone_label, "");
    }

    // Date and time (combined)
    if (!state.ntp_synced) {
        lv_label_set_text(m_date_label, "--");
    } else {
        time_t now = time(nullptr);
        struct tm tm_local;
#ifdef FORCE_TEST_DATE
        memset(&tm_local, 0, sizeof(tm_local));
        tm_local.tm_year = 125;  // 2025
        tm_local.tm_mon  = 11;   // December
        tm_local.tm_mday = 31;
        tm_local.tm_hour = 12;
        tm_local.tm_min  = 59;
        tm_local.tm_wday = 3;    // Wednesday (Dec 31 2025 is a Wednesday)
#else
        localtime_r(&now, &tm_local);
#endif
        build_date_string(buf, sizeof(buf), &lv_font_sourcesanspro_bold36, &tm_local);
        lv_label_set_text(m_date_label, buf);
    }

    checkStaleness(state);
}

void WeatherDisplay::checkStaleness(const WeatherState & state) {
    if (!state.outdoor_valid) {
        return;
    }
    int64_t age_sec = (esp_timer_get_time() - state.outdoor_captured_us) / 1'000'000LL;
    bool stale = age_sec > STALE_THRESHOLD_SEC;
    if (stale == m_is_stale) {
        return;
    }
    m_is_stale = stale;
    if (stale) {
        if (state.ntp_synced) {
            time_t reading_wall_time = time(nullptr) - (time_t)age_sec;
            struct tm tm_local;
            localtime_r(&reading_wall_time, &tm_local);
            int hour = tm_local.tm_hour % 12;
            if (hour == 0) {
                hour = 12;
            }
            char buf[32];
            snprintf(buf, sizeof(buf), "as of %d:%02d%s",
                hour, tm_local.tm_min, tm_local.tm_hour < 12 ? "am" : "pm");
            lv_label_set_text(m_stale_label, buf);
        } else {
            lv_label_set_text(m_stale_label, "stale");
        }
        lv_obj_set_style_text_color(m_stale_label, lv_color_white(), 0);
        lv_obj_set_style_bg_color(m_stale_label, COLOR_STALE_BG, 0);
        lv_obj_set_style_bg_opa(m_stale_label, LV_OPA_COVER, 0);
    } else {
        lv_label_set_text(m_stale_label, "");
        lv_obj_set_style_text_color(m_stale_label, COLOR_MUTED, 0);
        lv_obj_set_style_bg_opa(m_stale_label, LV_OPA_TRANSP, 0);
    }
}
