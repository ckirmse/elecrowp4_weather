#pragma once

#include <cstdint>

#include "signal_capture.h"

struct AcuriteReading {
    uint16_t sensor_id;
    char channel;       // 'A', 'B', or 'C'
    bool battery_ok;
    int humidity;       // %RH
    float temp_f;       // °F
    float temp_c;       // °C
};

// State machine that processes edge events from SignalCapture and emits
// validated AcuRite 592TXR readings.
//
// CC1101 GDO0 is inverted: LOW when OOK carrier present, HIGH when idle.
// The decoder therefore uses RISE edges (carrier-ON ends); pulse_us = carrier-ON duration.
//
// Observed timing from CC1101 async OOK output (carrier-ON duration):
//   preamble pulses: ~500–650 µs (varies with AGC / signal strength)
//   data short pulse: ~155–313 µs → bit 0
//   data long  pulse: ~328–542 µs → bit 1
//   preamble→data: first RISE pulse below PREAMBLE_IDLE_MIN_US
//   inter-message gap detected via long FALL pulse > RESET_THRESHOLD_US
//   7 data bytes, checksum = sum(bytes 0..5) & 0xFF == byte 6
//
// Thresholds for DATA decoding are computed adaptively from the measured
// preamble pulse average so they scale with CC1101 AGC compression.
//
// Reference: rtl_433 acurite.c / NorthernMan54/rtl_433_ESP
class AcuriteDecoder {
public:
    AcuriteDecoder();

    // Feed one edge event. Returns true (and fills *reading) when a valid
    // packet has been decoded.
    bool processEvent(const PulseEvent & ev, AcuriteReading * reading);

private:
    // Fixed lower bound to trigger IDLE→PREAMBLE.  Chosen to be above the
    // highest observed long data bit across all conditions (~542 µs max).
    static constexpr int64_t NOISE_FLOOR_US = 120;     // pulses below this ignored in preamble
    static constexpr int64_t PREAMBLE_IDLE_MIN_US = 500;
    static constexpr int64_t PREAMBLE_MAX_US = 750;  // above this, reject as noise/data
    static constexpr int MIN_PREAMBLE_COUNT = 3;
    static constexpr int64_t RESET_THRESHOLD_US = 2500;
    static constexpr int MSG_BYTES = 7;
    static constexpr int MSG_BITS = MSG_BYTES * 8;

    // Adaptive thresholds derived from preamble average on each PREAMBLE→DATA
    // transition.  Defaults are for a preamble avg of ~600 µs.
    //   data_threshold = preamble_avg * 55%   (midpoint short/long data pulse)
    //   data_max       = preamble_avg * 95%   (above this in DATA = spurious)
    static constexpr int PREAMBLE_DATA_THRESHOLD_PCT = 55;

    enum class State { IDLE, PREAMBLE, DATA };

    State m_state;
    int64_t m_last_edge_us;
    int m_preamble_count;
    int64_t m_preamble_sum_us;
    int64_t m_data_threshold_us;
    int m_bit_count;
    uint8_t m_bytes[MSG_BYTES];

    void reset();
    bool tryDecode(AcuriteReading * reading);
    bool decodeBytes(AcuriteReading * reading);
};
