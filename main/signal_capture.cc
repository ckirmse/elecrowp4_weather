#include "esp_timer.h"

#include "signal_capture.h"
#include "log_util.h"

static const char * TAG = "SignalCapture";

void IRAM_ATTR SignalCapture::gpioIsrHandler(void * arg) {
    SignalCapture * self = static_cast<SignalCapture *>(arg);

    int64_t ts = esp_timer_get_time();
    int level = gpio_get_level(self->m_pin);

    // Discard spurious re-triggers: a real edge always changes the level.
    // Double-fires from noise glitches read the same level as m_last_level.
    if (level == self->m_last_level) {
        return;
    }
    self->m_last_level = level;

    PulseEvent ev;
    ev.timestamp_us = ts;
    ev.level = level;

    BaseType_t higher_priority_task_woken = pdFALSE;
    xQueueSendFromISR(self->m_queue, &ev, &higher_priority_task_woken);

    if (higher_priority_task_woken) {
        portYIELD_FROM_ISR();
    }
}

SignalCapture::SignalCapture(): m_queue(nullptr), m_pin(GPIO_NUM_NC), m_last_level(0) {
}

void SignalCapture::init(gpio_num_t pin) {
    m_pin = pin;
    m_queue = xQueueCreate(QUEUE_DEPTH, sizeof(PulseEvent));

    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = GPIO_MODE_INPUT;
    cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_ANYEDGE;
    gpio_config(&cfg);

    m_last_level = gpio_get_level(pin);  // snapshot before ISR installed
    gpio_install_isr_service(0);
    gpio_isr_handler_add(pin, gpioIsrHandler, this);

    lprintf(TAG, "Signal capture on GPIO %d", (int)pin);
}

bool SignalCapture::tryGetEvent(PulseEvent * event) {
    return xQueueReceive(m_queue, event, 0) == pdTRUE;
}
