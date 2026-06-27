#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"

#include "acurite.h"
#include "bme680.h"
#include "cc1101.h"
#include "display.h"
#include "log_util.h"
#include "ntp_sync.h"
#include "signal_capture.h"
#include "weather_display.h"

static const char * TAG = "Main";

// ─── Logging helpers (CIC style) ────────────────────────────────────────────

void lprintf(const char * tag, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char full_fmt[256];
    snprintf(full_fmt, sizeof(full_fmt),
        "%s%s (%" PRIu32 ") %s: %s" LOG_RESET_COLOR "\n",
        LOG_COLOR_I, "I", esp_log_timestamp(), tag, fmt);
    esp_log_writev(ESP_LOG_INFO, tag, full_fmt, args);
    va_end(args);
}

void eprintf(const char * tag, const char * fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char full_fmt[256];
    snprintf(full_fmt, sizeof(full_fmt),
        "%s%s (%" PRIu32 ") %s: %s" LOG_RESET_COLOR "\n",
        LOG_COLOR_E, "E", esp_log_timestamp(), tag, fmt);
    esp_log_writev(ESP_LOG_ERROR, tag, full_fmt, args);
    va_end(args);
}

// ─── I2C bus scan ───────────────────────────────────────────────────────────

// Update these if the board's I2C pins differ.
static constexpr gpio_num_t I2C_SDA_PIN = GPIO_NUM_45;
static constexpr gpio_num_t I2C_SCL_PIN = GPIO_NUM_46;

static void scanI2c() {
    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = I2C_SDA_PIN;
    bus_cfg.scl_io_num = I2C_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;

    i2c_master_bus_handle_t bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&bus_cfg, &bus));

    lprintf(TAG, "I2C scan (SDA=GPIO%d SCL=GPIO%d):", (int)I2C_SDA_PIN, (int)I2C_SCL_PIN);
    int found = 0;
    // Suppress the IDF driver's per-address error logs; we report results ourselves.
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    // 0x00–0x07 and 0x78–0x7F are reserved in the I2C spec.
    for (uint8_t addr = 0x08; addr <= 0x77; addr++) {
        if (i2c_master_probe(bus, addr, 10) == ESP_OK) {
            lprintf(TAG, "  I2C device at 0x%02X", addr);
            found++;
        }
    }
    esp_log_level_set("i2c.master", ESP_LOG_WARN);
    lprintf(TAG, "I2C scan done: %d device(s) found", found);

    ESP_ERROR_CHECK(i2c_del_master_bus(bus));
}

// ─── Startup LVGL timer (drives rendering while NTP blocks run_task) ─────────

static char s_status_buf[64];
static volatile bool s_status_dirty;
static WeatherDisplay * s_weather_display_ptr;

static void on_ntp_status(const char * msg) {
    // Called from the ESP-IDF event-loop task — write to buffer only.
    strncpy(s_status_buf, msg, sizeof(s_status_buf) - 1);
    s_status_buf[sizeof(s_status_buf) - 1] = '\0';
    s_status_dirty = true;
}

static void startup_lvgl_cb(void *) {
    // Called from esp_timer task while run_task is blocked in ntpSyncTime.
    // Only task touching LVGL during this window, so no mutex needed.
    lv_tick_inc(20);
    if (s_status_dirty && s_weather_display_ptr) {
        s_weather_display_ptr->showStartupStatus(s_status_buf);
        s_status_dirty = false;
    }
    lv_timer_handler();
}

// ─── Main task ──────────────────────────────────────────────────────────────

static void run_task(void * arg) {
    Display display;
    display.init();

    WeatherDisplay weather_display;
    weather_display.init(display);

    // scanI2c();

    // Drive LVGL via periodic timer while ntpSyncTime blocks this task.
    s_weather_display_ptr = &weather_display;
    s_status_dirty = false;
    esp_timer_handle_t startup_timer;
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = startup_lvgl_cb;
    timer_args.name = "lvgl_startup";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &startup_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(startup_timer, 20'000));

    ntpSyncTime(on_ntp_status);

    ESP_ERROR_CHECK(esp_timer_stop(startup_timer));
    ESP_ERROR_CHECK(esp_timer_delete(startup_timer));
    s_weather_display_ptr = nullptr;

    // Brief pause so the final status message (e.g. "Time synced") is visible.
    vTaskDelay(pdMS_TO_TICKS(800));

    weather_display.showMainScreen();
    weather_display.showWaiting();
    // Advance LVGL past its refresh period (default ~33 ms) so the refresh
    // task fires immediately and the main screen is actually rendered before
    // hardware init begins.  The startup timer stopped driving ticks at NTP
    // completion, so without this the first lv_timer_handler in the main loop
    // sees 0 elapsed ticks and the refresh task doesn't run.
    lv_tick_inc(50);
    lv_timer_handler();

    weather_display.showStatus("Initializing BME680 sensor...");
    lv_tick_inc(50);
    lv_timer_handler();

    Bme680 bme680;
    bme680.init();

    weather_display.showWaiting();
    lv_tick_inc(50);
    lv_timer_handler();

    Cc1101 radio;
    radio.init();

    SignalCapture capture;
    capture.init(CC1101_PIN_GDO0);
    lprintf(TAG, "GDO0 (GPIO %d) level at startup: %d  (0=idle/no signal, 1=signal or floating)",
        (int)CC1101_PIN_GDO0, gpio_get_level(CC1101_PIN_GDO0));

    radio.startRx();

    if (!radio.isDetected()) {
        weather_display.showRadioError("CC1101 SPI error - check wiring");
        lv_tick_inc(50);
        lv_timer_handler();
    }

    AcuriteDecoder decoder;

    {
        Bme680Reading r;
        if (bme680.read(&r)) {
            float temp_f = r.temp_c * 9.0f / 5.0f + 32.0f;
            lprintf(TAG, "BME680 | Temp: %.1f F / %.1f C  Humidity: %.1f%%  Pressure: %.1f hPa  Gas: %.0f ohm",
                temp_f, r.temp_c, r.humidity_pct, r.pressure_hpa, r.gas_resistance_ohm);
        }
    }

    lprintf(TAG, "Entering main loop");

    int64_t last_tick_us = esp_timer_get_time();
    int64_t last_stats_us = esp_timer_get_time();
    int64_t last_bme_us = esp_timer_get_time();
    uint32_t edges_this_sec = 0;
    uint32_t edges_total = 0;
    uint32_t packets_total = 0;
    int64_t last_edge_us = 0;

    // Burst pulse-width capture: on the leading edge of each new burst, log
    // the first BURST_LOG pulses so we can tune the decoder thresholds.
    static constexpr int BURST_LOG = 30;
    int burst_log_remaining = 0;
    int64_t burst_prev_edge_us = 0;

    while (true) {
        int64_t now = esp_timer_get_time();

        // Advance the LVGL tick counter based on elapsed real time.
        uint32_t elapsed_ms = (uint32_t)((now - last_tick_us) / 1000);
        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
            last_tick_us = now;
        }

        // Drain all queued signal edges and feed to the AcuRite decoder.
        PulseEvent ev;
        while (capture.tryGetEvent(&ev)) {
            edges_this_sec++;
            edges_total++;
            last_edge_us = ev.timestamp_us;

            // Detect burst start: gap of >50 ms since the last edge.
            if (burst_prev_edge_us > 0) {
                int64_t gap_us = ev.timestamp_us - burst_prev_edge_us;
                if (gap_us > 50'000) {
                    // lprintf(TAG, "--- BURST START (silent for %lld ms) ---",
                    //     gap_us / 1000);
                    burst_log_remaining = BURST_LOG;
                }
                // Log individual pulse widths for the first BURST_LOG edges.
                if (burst_log_remaining > 0) {
                    // lprintf(TAG, "  p[%02d] %s %5lld us",
                    //     BURST_LOG - burst_log_remaining,
                    //     ev.level ? "  rise" : "FALL ",
                    //     gap_us);
                    burst_log_remaining--;
                }
            }
            burst_prev_edge_us = ev.timestamp_us;

            AcuriteReading reading;
            if (decoder.processEvent(ev, &reading)) {
                packets_total++;
                int rssi_dbm = radio.readRssi();
                lprintf(TAG,
                    "AcuRite | ID: %04X  Ch: %c  Bat: %s  "
                    "Temp: %.1f F / %.1f C  Humidity: %d%%  Signal: %d dBm",
                    reading.sensor_id,
                    reading.channel,
                    reading.battery_ok ? "OK" : "LOW",
                    reading.temp_f,
                    reading.temp_c,
                    reading.humidity,
                    rssi_dbm);

                weather_display.update(reading, rssi_dbm);
            }
        }

        // Once per second: log pulse activity and decoder state.
        if (now - last_stats_us >= 1'000'000) {
            // lprintf(TAG,
            //     "edges/s: %4" PRIu32 "  total: %6" PRIu32
            //     "  packets: %" PRIu32 "  last_edge: %" PRId64 " ms ago",
            //     edges_this_sec, edges_total, packets_total,
            //     last_edge_us > 0 ? (now - last_edge_us) / 1000 : -1);
            edges_this_sec = 0;
            last_stats_us  = now;

            weather_display.checkStaleness();
        }

        if (now - last_bme_us >= 10'000'000) {
            Bme680Reading r;
            if (bme680.read(&r)) {
                float temp_f = r.temp_c * 9.0f / 5.0f + 32.0f;
                lprintf(TAG, "BME680 | Temp: %.1f F / %.1f C  Humidity: %.1f%%  Gas: %.0f ohm",
                    temp_f, r.temp_c, r.humidity_pct, r.gas_resistance_ohm);
            }
            last_bme_us = now;
        }

        lv_timer_handler();
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ─── Entry point ────────────────────────────────────────────────────────────

extern "C" void app_main(void) {
    xTaskCreatePinnedToCore(
        run_task,
        "Run",
        3 * 8192,
        NULL,
        1,
        NULL,
        1);
}
