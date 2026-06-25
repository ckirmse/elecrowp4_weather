#pragma once

#include "driver/gpio.h"
#include "driver/spi_master.h"

// ─── CC1101 SPI pin assignments ─────────────────────────────────────────────
// CC1101 → ESP32-P4 wiring (Elecrow CrowPanel Advanced P4 header pins):
//   CC1101 SCK  → IO2   (CC1101_PIN_CLK)
//   CC1101 MOSI → IO3   (CC1101_PIN_MOSI)
//   CC1101 MISO → IO4   (CC1101_PIN_MISO)  ← also the CHIP_RDY/SO signal
//   CC1101 CSN  → IO5   (CC1101_PIN_CS)
//   CC1101 GDO0 → IO25  (CC1101_PIN_GDO0)  ← raw OOK demodulated output
//   CC1101 GDO2 → NC    (not used)
//   CC1101 VCC  → 3.3V
//   CC1101 GND  → GND
static constexpr gpio_num_t CC1101_PIN_CLK = GPIO_NUM_2;
static constexpr gpio_num_t CC1101_PIN_MOSI = GPIO_NUM_3;
static constexpr gpio_num_t CC1101_PIN_MISO = GPIO_NUM_4;   // also GDO1 / SO
static constexpr gpio_num_t CC1101_PIN_CS = GPIO_NUM_5;
static constexpr gpio_num_t CC1101_PIN_GDO0 = GPIO_NUM_25;  // async serial data out
// ────────────────────────────────────────────────────────────────────────────

class Cc1101 {
public:
    Cc1101();
    ~Cc1101() = default;

    void init();
    void startRx();
    int readRssi();
    bool isDetected() const { return m_detected; }

    uint8_t readReg(uint8_t addr);
    void writeReg(uint8_t addr, uint8_t value);
    void strobe(uint8_t cmd);

private:
    spi_device_handle_t m_spi;
    bool m_detected = false;

    void assertCs();
    void deassertCs();
    uint8_t transfer(uint8_t byte);
};

// CC1101 strobe commands
static constexpr uint8_t CC1101_SRES  = 0x30;
static constexpr uint8_t CC1101_SRX   = 0x34;
static constexpr uint8_t CC1101_SIDLE = 0x36;
static constexpr uint8_t CC1101_SFRX  = 0x3A;
static constexpr uint8_t CC1101_SNOP  = 0x3D;

// CC1101 register addresses
static constexpr uint8_t CC1101_REG_IOCFG2   = 0x00;
static constexpr uint8_t CC1101_REG_IOCFG1   = 0x01;
static constexpr uint8_t CC1101_REG_IOCFG0   = 0x02;
static constexpr uint8_t CC1101_REG_FIFOTHR  = 0x03;
static constexpr uint8_t CC1101_REG_PKTLEN   = 0x06;
static constexpr uint8_t CC1101_REG_PKTCTRL1 = 0x07;
static constexpr uint8_t CC1101_REG_PKTCTRL0 = 0x08;
static constexpr uint8_t CC1101_REG_CHANNR   = 0x0A;
static constexpr uint8_t CC1101_REG_FSCTRL1  = 0x0B;
static constexpr uint8_t CC1101_REG_FSCTRL0  = 0x0C;
static constexpr uint8_t CC1101_REG_FREQ2    = 0x0D;
static constexpr uint8_t CC1101_REG_FREQ1    = 0x0E;
static constexpr uint8_t CC1101_REG_FREQ0    = 0x0F;
static constexpr uint8_t CC1101_REG_MDMCFG4  = 0x10;
static constexpr uint8_t CC1101_REG_MDMCFG3  = 0x11;
static constexpr uint8_t CC1101_REG_MDMCFG2  = 0x12;
static constexpr uint8_t CC1101_REG_MDMCFG1  = 0x13;
static constexpr uint8_t CC1101_REG_MDMCFG0  = 0x14;
static constexpr uint8_t CC1101_REG_DEVIATN  = 0x15;
static constexpr uint8_t CC1101_REG_MCSM1    = 0x17;
static constexpr uint8_t CC1101_REG_MCSM0    = 0x18;
static constexpr uint8_t CC1101_REG_FOCCFG   = 0x19;
static constexpr uint8_t CC1101_REG_BSCFG    = 0x1A;
static constexpr uint8_t CC1101_REG_AGCCTRL2 = 0x1B;
static constexpr uint8_t CC1101_REG_AGCCTRL1 = 0x1C;
static constexpr uint8_t CC1101_REG_AGCCTRL0 = 0x1D;
static constexpr uint8_t CC1101_REG_WORCTRL  = 0x20;
static constexpr uint8_t CC1101_REG_FREND1   = 0x21;
static constexpr uint8_t CC1101_REG_FREND0   = 0x22;
static constexpr uint8_t CC1101_REG_FSCAL3   = 0x23;
static constexpr uint8_t CC1101_REG_FSCAL2   = 0x24;
static constexpr uint8_t CC1101_REG_FSCAL1   = 0x25;
static constexpr uint8_t CC1101_REG_FSCAL0   = 0x26;
static constexpr uint8_t CC1101_REG_RSSI      = 0x34;  // status reg (read via burst: addr|0xC0)
static constexpr uint8_t CC1101_REG_MARCSTATE = 0x35;  // status reg (read via burst: addr|0xC0)
static constexpr uint8_t CC1101_REG_VERSION   = 0xF1;  // status reg (read via burst)
