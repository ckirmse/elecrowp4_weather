#pragma once

#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "lvgl.h"

static constexpr int LCD_H_RES = 1024;
static constexpr int LCD_V_RES = 600;

class Display {
public:
    Display();
    ~Display() = default;

    void init();

    // Called from the LVGL flush callback to push the rendered buffer to the DPI controller.
    void renderFrame(void * px_map);

    lv_display_t * getLvDisplay() { return m_lv_display; }

private:
    esp_lcd_dsi_bus_handle_t m_dsi_bus;
    esp_lcd_panel_handle_t m_lcd;
    void * m_fb[2];
    lv_display_t * m_lv_display;

    void initDsi();
    void initBacklight();
    void initLvgl();
};
