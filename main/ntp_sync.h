#pragma once

// Status callback invoked at each stage of WiFi/NTP startup.
// Called from the ESP-IDF event-loop task — do not call LVGL directly.
using NtpStatusCb = void (*)(const char * msg);

// Connects to the WiFi network configured via menuconfig, syncs the system
// clock via SNTP (pool.ntp.org), then disconnects. Blocks until synced or
// timed out. Clock is set to UTC; timezone conversion is in weather_display.
// Returns true if the clock was successfully synced.
bool ntpSyncTime(NtpStatusCb cb = nullptr);
