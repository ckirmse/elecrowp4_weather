#pragma once

#include "driver/i2c_master.h"
#include "bme68x.h"

struct Bme680Reading {
    float temp_c;
    float humidity_pct;
    float gas_resistance_ohm;
};

class Bme680 {
public:
    Bme680();
    void init();
    bool read(Bme680Reading * out);
    bool isInitialized() const { return m_initialized; }

private:
    i2c_master_bus_handle_t m_bus;
    i2c_master_dev_handle_t m_dev;
    struct bme68x_dev m_sensor;
    struct bme68x_conf m_conf;
    bool m_initialized;
};
