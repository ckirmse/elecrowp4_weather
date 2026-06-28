#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
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
#include "weather_state.h"

static const char * TAG = "Main";

// ─── Logging helpers ─────────────────────────────────────────────────────────

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

// ─── Shared state ────────────────────────────────────────────────────────────

static WeatherState s_state;
static SemaphoreHandle_t s_state_mutex;
static TaskHandle_t s_ui_task_handle;

// Sensor objects — initialized in run_task (radio/CC1101) or bme_task (BME680)
// before the respective task starts accessing them.
static Cc1101 s_radio;
static SignalCapture s_capture;
static AcuriteDecoder s_decoder;
static Bme680 s_bme;

// ─── Startup LVGL timer (drives rendering while NTP blocks run_task) ──────────

static char s_status_buf[64];
static volatile bool s_status_dirty;
static WeatherDisplay * s_weather_display_ptr;

static void on_ntp_status(const char * msg) {
    strncpy(s_status_buf, msg, sizeof(s_status_buf) - 1);
    s_status_buf[sizeof(s_status_buf) - 1] = '\0';
    s_status_dirty = true;
}

static void startup_lvgl_cb(void *) {
    lv_tick_inc(20);
    if (s_status_dirty && s_weather_display_ptr) {
        s_weather_display_ptr->showStartupStatus(s_status_buf);
        s_status_dirty = false;
    }
    lv_timer_handler();
}

// ─── Clock timer: notifies UI task every second ───────────────────────────────

static void clock_timer_cb(void *) {
    xTaskNotify(s_ui_task_handle, 0, eNoAction);
}

// ─── Radio task: signal capture + AcuRite decode ─────────────────────────────

static void radio_task(void *) {
    while (true) {
        PulseEvent ev;
        while (s_capture.tryGetEvent(&ev)) {
            AcuriteReading reading;
            if (s_decoder.processEvent(ev, &reading)) {
                int rssi_dbm = s_radio.readRssi();
                lprintf(TAG,
                    "AcuRite | ID: %04X  Ch: %c  Bat: %s  "
                    "Temp: %.1f F / %.1f C  Humidity: %d%%  Signal: %d dBm",
                    reading.sensor_id, reading.channel,
                    reading.battery_ok ? "OK" : "LOW",
                    reading.temp_f, reading.temp_c,
                    reading.humidity, rssi_dbm);

                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_state.outdoor_valid = true;
                s_state.outdoor_temp_f = reading.temp_f;
                s_state.outdoor_temp_c = reading.temp_c;
                s_state.outdoor_humidity = reading.humidity;
                s_state.outdoor_sensor_id = reading.sensor_id;
                s_state.outdoor_channel = reading.channel;
                s_state.outdoor_battery_ok = reading.battery_ok;
                s_state.outdoor_rssi_dbm = rssi_dbm;
                s_state.outdoor_updated_at = time(nullptr);
                xSemaphoreGive(s_state_mutex);

                xTaskNotify(s_ui_task_handle, 0, eNoAction);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

// ─── BME680 task: indoor sensor, reads every 10 s ─────────────────────────────

static void bme_task(void *) {
    s_bme.init();

    while (true) {
        if (!s_bme.isInitialized()) {
            lprintf(TAG, "BME680 not initialized, retrying...");
            s_bme.init();
        }
        if (s_bme.isInitialized()) {
            Bme680Reading r;
            if (s_bme.read(&r)) {
                float temp_f = r.temp_c * 9.0f / 5.0f + 32.0f;
                lprintf(TAG, "BME680 | Temp: %.1f F / %.1f C  Humidity: %.1f%%  Pressure: %.1f hPa  Gas: %.0f ohm",
                    temp_f, r.temp_c, r.humidity_pct, r.pressure_hpa, r.gas_resistance_ohm);

                xSemaphoreTake(s_state_mutex, portMAX_DELAY);
                s_state.indoor_valid = true;
                s_state.indoor_temp_c = r.temp_c;
                s_state.indoor_temp_f = temp_f;
                s_state.indoor_humidity_pct = r.humidity_pct;
                s_state.indoor_pressure_hpa = r.pressure_hpa;
                s_state.indoor_gas_resistance_ohm = r.gas_resistance_ohm;
                xSemaphoreGive(s_state_mutex);

                xTaskNotify(s_ui_task_handle, 0, eNoAction);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ─── NTP retry task: retries once per minute until the clock is synced ───────

static void ntp_sync_task(void * arg) {
    bool synced = (bool)(uintptr_t)arg;
    while (true) {
        uint32_t delay_ms = synced ? 24u * 60u * 60u * 1000u : 60u * 1000u;
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
        lprintf(TAG, "%s NTP sync", synced ? "Daily" : "Retry");
        if (ntpSyncTime()) {
            synced = true;
        }
    }
}

// ─── UI task (run_task) ───────────────────────────────────────────────────────

static void run_task(void * arg) {
    s_ui_task_handle = xTaskGetCurrentTaskHandle();
    s_state_mutex = xSemaphoreCreateMutex();

    Display display;
    display.init();

    WeatherDisplay weather_display;
    weather_display.init(display);

    // Drive LVGL via periodic timer while ntpSyncTime blocks this task.
    s_weather_display_ptr = &weather_display;
    s_status_dirty = false;
    esp_timer_handle_t startup_timer;
    esp_timer_create_args_t timer_args = {};
    timer_args.callback = startup_lvgl_cb;
    timer_args.name = "lvgl_startup";
    ESP_ERROR_CHECK(esp_timer_create(&timer_args, &startup_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(startup_timer, 20'000));

    bool ntp_ok = ntpSyncTime(on_ntp_status);

    ESP_ERROR_CHECK(esp_timer_stop(startup_timer));
    ESP_ERROR_CHECK(esp_timer_delete(startup_timer));
    s_weather_display_ptr = nullptr;

    vTaskDelay(pdMS_TO_TICKS(800));

    weather_display.showMainScreen();
    weather_display.render(s_state);  // shows "--" everywhere (state is empty)
    lv_tick_inc(50);
    lv_timer_handler();

    // Init CC1101 radio before starting radio_task.
    s_radio.init();
    s_capture.init(CC1101_PIN_GDO0);
    lprintf(TAG, "GDO0 (GPIO %d) level at startup: %d  (0=idle/no signal, 1=signal or floating)",
        (int)CC1101_PIN_GDO0, gpio_get_level(CC1101_PIN_GDO0));
    s_radio.startRx();

    if (!s_radio.isDetected()) {
        weather_display.showRadioError("CC1101 SPI error - check wiring");
        lv_tick_inc(50);
        lv_timer_handler();
    }

    // Start sensor tasks.
    xTaskCreate(radio_task, "Radio", 2 * 8192, nullptr, 5, nullptr);
    xTaskCreate(bme_task, "BME", 2 * 8192, nullptr, 2, nullptr);
    if (strlen(CONFIG_WIFI_SSID) > 0) {
        xTaskCreate(ntp_sync_task, "NtpSync", 4 * 8192, (void *)(uintptr_t)ntp_ok, 1, nullptr);
    }

    // 1-second clock timer.
    esp_timer_handle_t clock_timer;
    esp_timer_create_args_t clock_args = {};
    clock_args.callback = clock_timer_cb;
    clock_args.name = "clock";
    ESP_ERROR_CHECK(esp_timer_create(&clock_args, &clock_timer));
    ESP_ERROR_CHECK(esp_timer_start_periodic(clock_timer, 1'000'000));

    lprintf(TAG, "Entering UI loop");

    int64_t last_tick_us = esp_timer_get_time();

    while (true) {
        uint32_t notif_val = 0;
        BaseType_t was_notified = xTaskNotifyWait(0, 0xFFFFFFFF, &notif_val, pdMS_TO_TICKS(5));

        int64_t now = esp_timer_get_time();
        uint32_t elapsed_ms = (uint32_t)((now - last_tick_us) / 1000);
        if (elapsed_ms > 0) {
            lv_tick_inc(elapsed_ms);
            last_tick_us = now;
        }

        if (was_notified) {
            WeatherState snapshot;
            xSemaphoreTake(s_state_mutex, portMAX_DELAY);
            snapshot = s_state;
            xSemaphoreGive(s_state_mutex);
            weather_display.render(snapshot);
        }

        lv_timer_handler();
    }
}

// ─── Entry point ─────────────────────────────────────────────────────────────

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
