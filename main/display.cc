#include <algorithm>
#include <memory.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"

#include "display.h"
#include "log_util.h"

// Backlight — same pins as Elecrow CrowPanel Advanced P4.
static constexpr gpio_num_t LCD_BK_POWER_PIN = GPIO_NUM_29;
static constexpr gpio_num_t LCD_BK_PWM_PIN = GPIO_NUM_31;
static constexpr uint32_t LCD_BK_PWM_FREQ_HZ = 30000;

// EK79007 1024x600 @60Hz timings (matches CIC initDeviceConfigElecrowP4).
static constexpr int HSYNC_BACK_PORCH = 160;
static constexpr int HSYNC_FRONT_PORCH = 160;
static constexpr int HSYNC_PULSE_WIDTH = 10;
static constexpr int VSYNC_BACK_PORCH = 23;
static constexpr int VSYNC_FRONT_PORCH = 12;
static constexpr int VSYNC_PULSE_WIDTH = 1;

static const char * TAG = "Display";

static Display * g_display = nullptr;

static void lvgl_flush_cb(lv_display_t * disp, const lv_area_t * area, uint8_t * px_map) {
    if (lv_display_flush_is_last(disp)) {
        g_display->renderFrame(px_map);
    }
    lv_display_flush_ready(disp);
}

Display::Display():
    m_dsi_bus(nullptr),
    m_lcd(nullptr),
    m_fb{nullptr, nullptr},
    m_lv_display(nullptr) {
}

void Display::init() {
    g_display = this;
    initDsi();
    initBacklight();
    initLvgl();
}

void Display::initDsi() {
    lprintf(TAG, "Enabling DSI PHY LDO");
    esp_ldo_channel_handle_t ldo_phy_chan = nullptr;
    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = 3;
    ldo_cfg.voltage_mv = 2500;
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_cfg, &ldo_phy_chan));

    lprintf(TAG, "Creating DSI bus");
    esp_lcd_dsi_bus_config_t dsi_bus_cfg;
    memset(&dsi_bus_cfg, 0, sizeof(dsi_bus_cfg));
    dsi_bus_cfg.bus_id = 0;
    dsi_bus_cfg.num_data_lanes = 2;
    dsi_bus_cfg.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    dsi_bus_cfg.lane_bit_rate_mbps = 900;
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&dsi_bus_cfg, &m_dsi_bus));

    lprintf(TAG, "Creating DBI IO");
    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_dbi_io_config_t dbi_cfg;
    memset(&dbi_cfg, 0, sizeof(dbi_cfg));
    dbi_cfg.virtual_channel = 0;
    dbi_cfg.lcd_cmd_bits = 8;
    dbi_cfg.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(m_dsi_bus, &dbi_cfg, &dbi_io));

    lprintf(TAG, "Creating EK79007 DPI panel");
    esp_lcd_dpi_panel_config_t dpi_cfg;
    memset(&dpi_cfg, 0, sizeof(dpi_cfg));
    dpi_cfg.virtual_channel = 0;
    dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz = 52;
    dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.out_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.num_fbs = 2;
    dpi_cfg.video_timing.h_size = LCD_H_RES;
    dpi_cfg.video_timing.v_size = LCD_V_RES;
    dpi_cfg.video_timing.hsync_pulse_width = HSYNC_PULSE_WIDTH;
    dpi_cfg.video_timing.hsync_back_porch  = HSYNC_BACK_PORCH;
    dpi_cfg.video_timing.hsync_front_porch = HSYNC_FRONT_PORCH;
    dpi_cfg.video_timing.vsync_pulse_width = VSYNC_PULSE_WIDTH;
    dpi_cfg.video_timing.vsync_back_porch  = VSYNC_BACK_PORCH;
    dpi_cfg.video_timing.vsync_front_porch = VSYNC_FRONT_PORCH;

    ek79007_vendor_config_t vendor_cfg;
    memset(&vendor_cfg, 0, sizeof(vendor_cfg));
    vendor_cfg.mipi_config.dsi_bus = m_dsi_bus;
    vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
    vendor_cfg.mipi_config.lane_num = 2;

    esp_lcd_panel_dev_config_t panel_cfg;
    memset(&panel_cfg, 0, sizeof(panel_cfg));
    panel_cfg.reset_gpio_num = GPIO_NUM_NC;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &vendor_cfg;

    ESP_ERROR_CHECK(esp_lcd_new_panel_ek79007(dbi_io, &panel_cfg, &m_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(m_lcd));
    ESP_ERROR_CHECK(esp_lcd_panel_init(m_lcd));

    ESP_ERROR_CHECK(esp_lcd_dpi_panel_get_frame_buffer(m_lcd, 2, &m_fb[0], &m_fb[1]));
    lprintf(TAG, "DSI display initialized %dx%d, fb[0]=%p fb[1]=%p",
        LCD_H_RES, LCD_V_RES, m_fb[0], m_fb[1]);
}

void Display::initBacklight() {
    gpio_set_direction(LCD_BK_POWER_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(LCD_BK_POWER_PIN, 1);

    ledc_timer_config_t timer_cfg = {};
    timer_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    timer_cfg.duty_resolution = LEDC_TIMER_11_BIT;
    timer_cfg.timer_num = LEDC_TIMER_0;
    timer_cfg.freq_hz = LCD_BK_PWM_FREQ_HZ;
    timer_cfg.clk_cfg = LEDC_USE_PLL_DIV_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer_cfg));

    ledc_channel_config_t channel_cfg = {};
    channel_cfg.gpio_num = LCD_BK_PWM_PIN;
    channel_cfg.speed_mode = LEDC_LOW_SPEED_MODE;
    channel_cfg.channel = LEDC_CHANNEL_0;
    channel_cfg.timer_sel = LEDC_TIMER_0;
    channel_cfg.duty = 2047;  // full brightness
    channel_cfg.hpoint = 0;
    ESP_ERROR_CHECK(ledc_channel_config(&channel_cfg));
}

void Display::initLvgl() {
    lprintf(TAG, "Initializing LVGL");
    lv_init();

    m_lv_display = lv_display_create(LCD_H_RES, LCD_V_RES);
    lv_display_set_user_data(m_lv_display, this);

    uint32_t fb_bytes = LCD_H_RES * LCD_V_RES * sizeof(uint16_t);
    lv_display_set_buffers(m_lv_display, m_fb[0], m_fb[1], fb_bytes,
        LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(m_lv_display, lvgl_flush_cb);

    lprintf(TAG, "LVGL initialized");
}

void Display::renderFrame(void * px_map) {
    ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(m_lcd, 0, 0, LCD_H_RES, LCD_V_RES, px_map));
}
