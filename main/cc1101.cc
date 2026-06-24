#include <string.h>

#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cc1101.h"
#include "log_util.h"

static const char * TAG = "CC1101";

// Register configuration for 433.92 MHz OOK async receive.
// Register values based on RadioLib / SmartRC-CC1101 reference configuration
// (see NorthernMan54/rtl_433_ESP for the Arduino equivalent).
// In async serial mode (PKTCTRL0 = 0x32), GDO0 outputs the raw demodulated
// OOK signal — connect it to CC1101_PIN_GDO0 for edge-timing capture.
static const uint8_t INIT_REGS[][2] = {
    {CC1101_REG_IOCFG2,   0x0D},  // GDO2: synchronous serial data
    {CC1101_REG_IOCFG0,   0x0D},  // GDO0: synchronous serial data (async OOK output)
    {CC1101_REG_FIFOTHR,  0x47},
    {CC1101_REG_PKTLEN,   0xFF},
    {CC1101_REG_PKTCTRL1, 0x00},  // no status byte, no addr check
    {CC1101_REG_PKTCTRL0, 0x32},  // async serial mode, no CRC, infinite packet
    {CC1101_REG_CHANNR,   0x00},
    {CC1101_REG_FSCTRL1,  0x06},
    {CC1101_REG_FSCTRL0,  0x00},
    {CC1101_REG_FREQ2,    0x10},  // 433.92 MHz: F = 0x10B071 * 26MHz / 2^16
    {CC1101_REG_FREQ1,    0xB0},
    {CC1101_REG_FREQ0,    0x71},
    {CC1101_REG_MDMCFG4,  0x87},  // RX BW ~200 kHz, DRATE_E = 7
    {CC1101_REG_MDMCFG3,  0x4A},  // DRATE_M = 74 → ~3.2 kBaud
    {CC1101_REG_MDMCFG2,  0x30},  // OOK, no preamble/sync detection
    {CC1101_REG_MDMCFG1,  0x22},
    {CC1101_REG_MDMCFG0,  0xF8},
    {CC1101_REG_DEVIATN,  0x15},
    {CC1101_REG_MCSM1,    0x30},  // stay in RX after packet
    {CC1101_REG_MCSM0,    0x18},  // auto-cal on idle→RX
    {CC1101_REG_FOCCFG,   0x14},  // disable frequency offset compensation for OOK
    {CC1101_REG_BSCFG,    0x6C},
    {CC1101_REG_AGCCTRL2, 0x43},  // DVGA limited to -6 dB step, max LNA, 33 dBm target
    {CC1101_REG_AGCCTRL1, 0x49},
    {CC1101_REG_AGCCTRL0, 0x91},  // 32-sample AGC filter
    {CC1101_REG_WORCTRL,  0xFB},
    {CC1101_REG_FREND1,   0x56},
    {CC1101_REG_FREND0,   0x11},  // OOK PA: 2 PATABLE entries
    {CC1101_REG_FSCAL3,   0xE9},
    {CC1101_REG_FSCAL2,   0x2A},
    {CC1101_REG_FSCAL1,   0x00},
    {CC1101_REG_FSCAL0,   0x1F},
};

Cc1101::Cc1101(): m_spi(nullptr) {
}

void Cc1101::init() {
    lprintf(TAG, "Initializing SPI bus");
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = CC1101_PIN_MOSI;
    bus_cfg.miso_io_num = CC1101_PIN_MISO;
    bus_cfg.sclk_io_num = CC1101_PIN_CLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 64;
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.mode = 0;                       // CC1101: CPOL=0, CPHA=0
    dev_cfg.clock_speed_hz = 4 * 1000 * 1000;
    dev_cfg.spics_io_num = -1;              // manual CS so we can poll MISO for ready
    dev_cfg.queue_size = 1;
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &dev_cfg, &m_spi));

    gpio_set_direction(CC1101_PIN_CS, GPIO_MODE_OUTPUT);
    gpio_set_level(CC1101_PIN_CS, 1);

    lprintf(TAG, "Resetting CC1101");
    strobe(CC1101_SRES);
    vTaskDelay(pdMS_TO_TICKS(10));

    lprintf(TAG, "Configuring CC1101 registers");
    for (auto & reg : INIT_REGS) {
        writeReg(reg[0], reg[1]);
    }

    // Verify chip by reading the VERSION status register (expected: 0x14 for CC1101).
    uint8_t version = readReg(CC1101_REG_VERSION | 0xC0);  // burst-read status reg
    lprintf(TAG, "CC1101 version register: 0x%02X (expected 0x14)", version);
}

void Cc1101::startRx() {
    strobe(CC1101_SIDLE);
    strobe(CC1101_SFRX);
    strobe(CC1101_SRX);

    // Read back key registers to verify SPI writes landed correctly.
    uint8_t pktctrl0  = readReg(CC1101_REG_PKTCTRL0);
    uint8_t iocfg0 = readReg(CC1101_REG_IOCFG0);
    uint8_t freq2 = readReg(CC1101_REG_FREQ2);
    uint8_t mdmcfg2 = readReg(CC1101_REG_MDMCFG2);
    // Wait for calibration to finish before reading MARCSTATE.
    // STARTCAL (0x08) → ENDCAL → RX takes up to ~800 µs.
    vTaskDelay(pdMS_TO_TICKS(5));
    // MARCSTATE is a status register — must use burst (addr|0xC0) access.
    uint8_t marcstate = readReg(CC1101_REG_MARCSTATE | 0xC0);

    lprintf(TAG, "PKTCTRL0=0x%02X(want 0x32)  IOCFG0=0x%02X(want 0x0D)  "
                 "FREQ2=0x%02X(want 0x10)  MDMCFG2=0x%02X(want 0x30)",
        pktctrl0, iocfg0, freq2, mdmcfg2);
    lprintf(TAG, "MARCSTATE=0x%02X (0x0D=RX, 0x01=IDLE, 0x00=SLEEP)", marcstate);
    lprintf(TAG, "CC1101 RX started");
}

int Cc1101::readRssi() {
    uint8_t raw = readReg(CC1101_REG_RSSI | 0xC0);
    return (int8_t)raw / 2 - 74;
}

void Cc1101::assertCs() {
    gpio_set_level(CC1101_PIN_CS, 0);
    // Wait for MISO (GDO1/SO) to go low — indicates crystal osc is stable.
    // After a reset and initial settle, this returns almost immediately.
    int timeout = 1000;
    while (gpio_get_level(CC1101_PIN_MISO) && --timeout > 0) {
        esp_rom_delay_us(1);
    }
}

void Cc1101::deassertCs() {
    gpio_set_level(CC1101_PIN_CS, 1);
}

uint8_t Cc1101::transfer(uint8_t byte) {
    spi_transaction_t t = {};
    t.length = 8;
    t.flags = SPI_TRANS_USE_TXDATA | SPI_TRANS_USE_RXDATA;
    t.tx_data[0] = byte;
    ESP_ERROR_CHECK(spi_device_polling_transmit(m_spi, &t));
    return t.rx_data[0];
}

void Cc1101::writeReg(uint8_t addr, uint8_t value) {
    assertCs();
    transfer(addr & 0x3F);   // write: bit7=0, bit6=0 (single), addr in [5:0]
    transfer(value);
    deassertCs();
}

uint8_t Cc1101::readReg(uint8_t addr) {
    assertCs();
    transfer(addr | 0x80);   // read: bit7=1, bit6=0 (single)
    uint8_t val = transfer(0x00);
    deassertCs();
    return val;
}

void Cc1101::strobe(uint8_t cmd) {
    assertCs();
    transfer(cmd & 0x3F);
    deassertCs();
}
