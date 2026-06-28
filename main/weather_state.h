#pragma once

#include <cstdint>
#include <time.h>

struct WeatherState {
    // Outdoor (AcuRite) — valid when outdoor_valid == true
    bool outdoor_valid = false;
    float outdoor_temp_f = 0;
    float outdoor_temp_c = 0;
    int outdoor_humidity = 0;
    uint16_t outdoor_sensor_id = 0;
    char outdoor_channel = 0;
    bool outdoor_battery_ok = false;
    int outdoor_rssi_dbm = 0;
    time_t outdoor_updated_at = 0;

    // Indoor (BME680) — valid when indoor_valid == true
    bool indoor_valid = false;
    float indoor_temp_c = 0;
    float indoor_temp_f = 0;
    float indoor_humidity_pct = 0;
    float indoor_pressure_hpa = 0;
    float indoor_gas_resistance_ohm = 0;
};
