#pragma once

#include <cstdint>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "driver/gpio.h"

// A single edge event from the CC1101 GDO0 pin.
struct PulseEvent {
    int64_t timestamp_us;   // from esp_timer_get_time()
    int     level;          // 0 = falling edge (signal end), 1 = rising edge (signal start)
};

class SignalCapture {
public:
    SignalCapture();
    ~SignalCapture() = default;

    void init(gpio_num_t pin);

    // Non-blocking: returns true and fills *event if a pulse edge is available.
    bool tryGetEvent(PulseEvent * event);

private:
    static constexpr int QUEUE_DEPTH = 256;

    QueueHandle_t  m_queue;
    gpio_num_t     m_pin;
    volatile int   m_last_level;  // toggled on each ISR fire; avoids gpio_get_level() race

    static void IRAM_ATTR gpioIsrHandler(void * arg);
};
