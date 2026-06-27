#include <string.h>

#include "driver/i2c_master.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bme68x.h"
#include "bme680.h"
#include "log_util.h"

static const char * TAG = "Bme680";

static constexpr gpio_num_t BME680_SDA_PIN   = GPIO_NUM_45;
static constexpr gpio_num_t BME680_SCL_PIN   = GPIO_NUM_46;
static constexpr uint8_t    BME680_I2C_ADDR  = 0x77;

static BME68X_INTF_RET_TYPE i2c_read_cb(uint8_t reg_addr, uint8_t * reg_data, uint32_t len, void * intf_ptr) {
    i2c_master_dev_handle_t dev = *(i2c_master_dev_handle_t *)intf_ptr;
    esp_err_t err = i2c_master_transmit_receive(dev, &reg_addr, 1, reg_data, len, 100);
    return (err == ESP_OK) ? BME68X_OK : BME68X_E_COM_FAIL;
}

static BME68X_INTF_RET_TYPE i2c_write_cb(uint8_t reg_addr, const uint8_t * reg_data, uint32_t len, void * intf_ptr) {
    i2c_master_dev_handle_t dev = *(i2c_master_dev_handle_t *)intf_ptr;
    uint8_t buf[32];
    if (len + 1 > sizeof(buf)) {
        return BME68X_E_COM_FAIL;
    }
    buf[0] = reg_addr;
    memcpy(&buf[1], reg_data, len);
    esp_err_t err = i2c_master_transmit(dev, buf, len + 1, 100);
    return (err == ESP_OK) ? BME68X_OK : BME68X_E_COM_FAIL;
}

static void delay_us_cb(uint32_t period_us, void * intf_ptr) {
    // Use vTaskDelay for long waits to stay cooperative; busy-wait for short ones.
    if (period_us >= 1000) {
        vTaskDelay(pdMS_TO_TICKS((period_us + 999) / 1000));
    } else {
        esp_rom_delay_us(period_us);
    }
}

Bme680::Bme680() :
    m_bus(nullptr),
    m_dev(nullptr),
    m_initialized(false) {
    memset(&m_sensor, 0, sizeof(m_sensor));
    memset(&m_conf, 0, sizeof(m_conf));
}

void Bme680::init() {
    if (m_initialized) {
        return;
    }

    if (m_dev) {
        i2c_master_bus_rm_device(m_dev);
        m_dev = nullptr;
    }
    if (m_bus) {
        i2c_del_master_bus(m_bus);
        m_bus = nullptr;
    }

    i2c_master_bus_config_t bus_cfg = {};
    bus_cfg.i2c_port = I2C_NUM_0;
    bus_cfg.sda_io_num = BME680_SDA_PIN;
    bus_cfg.scl_io_num = BME680_SCL_PIN;
    bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_cfg.glitch_ignore_cnt = 7;
    bus_cfg.flags.enable_internal_pullup = true;
    if (i2c_new_master_bus(&bus_cfg, &m_bus) != ESP_OK) {
        eprintf(TAG, "i2c_new_master_bus failed");
        return;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = BME680_I2C_ADDR;
    dev_cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(m_bus, &dev_cfg, &m_dev) != ESP_OK) {
        eprintf(TAG, "i2c_master_bus_add_device failed");
        return;
    }

    m_sensor.intf     = BME68X_I2C_INTF;
    m_sensor.intf_ptr = &m_dev;
    m_sensor.read     = i2c_read_cb;
    m_sensor.write    = i2c_write_cb;
    m_sensor.delay_us = delay_us_cb;
    m_sensor.amb_temp = 25;

    int8_t rslt = bme68x_init(&m_sensor);
    if (rslt != BME68X_OK) {
        eprintf(TAG, "bme68x_init failed: %d", rslt);
        return;
    }

    m_conf.os_hum  = BME68X_OS_1X;
    m_conf.os_temp = BME68X_OS_2X;
    m_conf.os_pres = BME68X_OS_1X;
    m_conf.filter  = BME68X_FILTER_OFF;
    m_conf.odr     = BME68X_ODR_NONE;
    rslt = bme68x_set_conf(&m_conf, &m_sensor);
    if (rslt != BME68X_OK) {
        eprintf(TAG, "bme68x_set_conf failed: %d", rslt);
        return;
    }

    struct bme68x_heatr_conf heatr_cfg = {};
    heatr_cfg.enable = BME68X_ENABLE;
    heatr_cfg.heatr_temp = 300;   // degrees C
    heatr_cfg.heatr_dur = 100;    // milliseconds
    rslt = bme68x_set_heatr_conf(BME68X_FORCED_MODE, &heatr_cfg, &m_sensor);
    if (rslt != BME68X_OK) {
        eprintf(TAG, "bme68x_set_heatr_conf failed: %d", rslt);
        return;
    }

    m_initialized = true;
    lprintf(TAG, "BME680 initialized (chip_id=0x%02X)", m_sensor.chip_id);
}

bool Bme680::read(Bme680Reading * out) {
    if (!m_initialized) {
        return false;
    }

    int8_t rslt = bme68x_set_op_mode(BME68X_FORCED_MODE, &m_sensor);
    if (rslt != BME68X_OK) {
        eprintf(TAG, "bme68x_set_op_mode failed: %d", rslt);
        return false;
    }

    uint32_t meas_dur_us = bme68x_get_meas_dur(BME68X_FORCED_MODE, &m_conf, &m_sensor);
    m_sensor.delay_us(meas_dur_us, m_sensor.intf_ptr);

    struct bme68x_data data;
    uint8_t n_fields = 0;
    rslt = bme68x_get_data(BME68X_FORCED_MODE, &data, &n_fields, &m_sensor);
    if (rslt != BME68X_OK || n_fields == 0 || !(data.status & BME68X_NEW_DATA_MSK)) {
        return false;
    }

    out->temp_c = data.temperature;
    out->humidity_pct = data.humidity;
    out->pressure_hpa = data.pressure / 100.0f;
    out->gas_resistance_ohm = data.gas_resistance;
    return true;
}
