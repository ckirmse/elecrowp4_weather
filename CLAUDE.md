# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

This is an ESP32-P4 weather station that receives AcuRite 433 MHz sensor data (temperature and humidity) via a CC1101 OOK radio, logs readings to the serial console, and displays them in large numbers on a 1024×600 MIPI-DSI display using LVGL. It runs on the Elecrow CrowPanel Advanced 7" P4 board.

## Build Commands

```bash
source $HOME/esp/esp-idf/export.sh
export IDF_TARGET=esp32p4
idf.py -B build -DSDKCONFIG=build/sdkconfig reconfigure
idf.py -B build -DSDKCONFIG=build/sdkconfig build
idf.py -B build -DSDKCONFIG=build/sdkconfig flash
idf.py -B build -DSDKCONFIG=build/sdkconfig monitor
```

The `-DSDKCONFIG=` flag keeps the generated sdkconfig isolated inside the build directory, matching CIC project convention for P4.

No tests or linter are configured.

### WiFi / NTP credentials

WiFi SSID and password are set via Kconfig (not hardcoded). Before first build:

```bash
idf.py -B build menuconfig   # → "Weather Station NTP" menu
```

Leave SSID empty to skip NTP sync entirely. Credentials are stored in `build/sdkconfig` (not committed).

### Managed components

`managed_components/` is auto-populated by the IDF component manager from `main/idf_component.yml`. Do not edit files there directly. `dependencies.lock` should be committed for reproducible builds.

## Hardware

**Board:** Elecrow CrowPanel Advanced 7" P4 (ESP32-P4, 32MB PSRAM, 16MB flash, 1024×600 MIPI-DSI display via EK79007)

**WiFi:** The ESP32-P4 has no native WiFi. The board includes an ESP32-C6 companion chip connected via 1-bit SDIO (slot 1). This is handled by `esp_hosted` + `esp_wifi_remote` components, configured in `sdkconfig.defaults`. WiFi behaves like normal `esp_wifi` from application code, but if WiFi is broken, investigate the SDIO transport (GPIO reset pin 32, CLK/CMD/D0 on pins 18/19/14).

**CC1101 wiring** (update `main/cc1101.h` if pins change):

| CC1101 pin | Board pin | GPIO |
|------------|-----------|------|
| SCK        | IO2       | 2    |
| MOSI (SI)  | IO3       | 3    |
| MISO (SO)  | IO4       | 4    |
| CSN        | IO5       | 5    |
| GDO0       | IO25      | 25   |
| GDO2       | NC        | —    |
| VCC        | 3.3V      | —    |
| GND        | GND       | —    |

Note: many cheap CC1101 modules are clones that return `0x0F` from the VERSION register instead of the genuine TI value `0x14`. They work fine for OOK receive.

## Architecture

### Startup sequence

`app_main` creates `run_task` pinned to core 1, which performs all initialization in order:

1. `Display::init()` — MIPI-DSI + LVGL
2. `WeatherDisplay::init()` — builds two LVGL screens: a startup screen (loaded immediately) and the main weather screen (not yet visible)
3. A 20 ms periodic `esp_timer` (`startup_lvgl_cb`) is started to drive `lv_tick_inc` + `lv_timer_handler` while `run_task` blocks
4. `ntpSyncTime()` — WiFi connect → get IP → SNTP sync → WiFi down; fires a status callback at each stage
5. The callback writes to `s_status_buf` / `s_status_dirty`; `startup_lvgl_cb` picks it up on the next tick and calls `WeatherDisplay::showStartupStatus()`. **LVGL is only ever touched from the timer task during this phase** — no mutex required.
6. Timer stops; 800 ms pause so the final status is readable; `WeatherDisplay::showMainScreen()` loads the weather screen
7. CC1101 and SignalCapture init, then the main loop runs

### Main loop cadence (core 1, 5 ms tick)

```
while (true) {
    lv_tick_inc(elapsed_ms);         // advance LVGL clock
    drain SignalCapture queue        // feed edges to AcuriteDecoder
        → on valid packet: lprintf + weather_display.update()
    once/second: log pulse stats     // edges/s, total, packets, last_edge age
    lv_timer_handler();              // drive LVGL rendering
    vTaskDelay(5 ms);
}
```

### Signal path: CC1101 → decoded reading

1. **CC1101** (`main/cc1101.h/.cc`) — SPI driver (manual CS, SPI2_HOST). Configured for 433.92 MHz OOK async serial mode (`PKTCTRL0=0x32`): GDO0 outputs the raw demodulated OOK signal.
2. **SignalCapture** (`main/signal_capture.h/.cc`) — GPIO ISR on GDO0 (both edges). Timestamps each edge with `esp_timer_get_time()` and pushes a `PulseEvent` to a FreeRTOS queue (depth 256).
3. **AcuriteDecoder** (`main/acurite.h/.cc`) — State machine (IDLE → PREAMBLE → DATA). Key subtleties:
   - **GDO0 polarity is inverted**: LOW = OOK carrier present, HIGH = idle. The decoder therefore acts on RISE edges (carrier-ON ends), and `pulse_us` is the carrier-ON duration.
   - **Adaptive thresholds**: during PREAMBLE, the decoder accumulates a running average of pulse widths. On transition to DATA, it sets `data_threshold_us = avg * 55%` to split short (bit-0) from long (bit-1) pulses. This makes the decoder robust to CC1101 AGC compression.
   - 7 bytes per packet; checksum = `sum(bytes[0..5]) & 0xFF == bytes[6]`.
4. **Display** (`main/display.h/.cc`) — Initialises the MIPI-DSI bus, EK79007 panel, and LVGL 9 with two PSRAM-backed frame buffers in `LV_DISPLAY_RENDER_MODE_DIRECT`. Flush callback calls `esp_lcd_panel_draw_bitmap`.
5. **WeatherDisplay** (`main/weather_display.h/.cc`) — LVGL UI. Two screens: startup (centered status label) and main (temperature + humidity panels). Timezone is hardcoded to `PST8PDT` in `WeatherDisplay::init()`.

### IDF 6.0 component notes

In IDF 6.0 vs 5.x, two components were absorbed into larger ones:
- `esp_lcd_mipi_dsi` → now part of `esp_lcd` (DSI sources compiled in for P4 target)
- `esp_ldo` → now part of `esp_hw_support` (LDO sources in `esp_hw_support/ldo/`)

`main/CMakeLists.txt` REQUIRES `esp_lcd` and `esp_hw_support` accordingly.

## Key Files

| File | Purpose |
|------|---------|
| `sdkconfig.defaults` | ESP32-P4 Kconfig defaults — PSRAM, LVGL, WiFi via C6/SDIO companion |
| `main/Kconfig.projbuild` | Adds "Weather Station NTP" menu (WiFi SSID/password) |
| `main/idf_component.yml` | Managed components: `esp_lcd_ek79007`, `lvgl 9.5`, `esp_hosted`, `esp_wifi_remote` |
| `main/cc1101.h` | **CC1101 pin definitions** — update these when rewiring |
| `main/acurite.cc` | AcuRite 592TXR OOK-PWM decoder; checksum, packet layout, adaptive thresholds |
| `main/ntp_sync.cc` | WiFi connect + SNTP sync; calls `NtpStatusCb` at each stage |
| `main/lv_font_sourcesansprobold80.c` | Large digit font, copied from `~/cic/src/core/lvglfonts/` |

## Coding Standards

Follows the conventions of the `~/cic` project:
- C++17, classes with `m_` prefix for member variables
- `static const char * TAG` per translation unit
- `lprintf(TAG, ...)` / `eprintf(TAG, ...)` wrappers (defined in `main.cc`, declared in `log_util.h`)
- `ESP_ERROR_CHECK(...)` for ESP-IDF calls
- `memset` to zero-init structs before filling fields
- `#pragma once` header guards
- No docstring blocks; comments only for non-obvious WHY
- Always brace `if`/`for`/`while` bodies — never on the same line as the keyword, never without braces
- Time-unit suffixes: use `_SEC` for seconds, `_MS` for milliseconds, `_US` for microseconds
