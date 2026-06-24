#include <string.h>

#include "esp_log.h"

#include "acurite.h"
#include "log_util.h"

static const char * TAG = "Acurite";

AcuriteDecoder::AcuriteDecoder():
    m_state(State::IDLE),
    m_last_edge_us(0),
    m_preamble_count(0),
    m_preamble_sum_us(0),
    m_data_threshold_us(330),
    m_bit_count(0) {
    memset(m_bytes, 0, sizeof(m_bytes));
}

void AcuriteDecoder::reset() {
    m_state = State::IDLE;
    m_preamble_count = 0;
    m_preamble_sum_us = 0;
    m_bit_count = 0;
    memset(m_bytes, 0, sizeof(m_bytes));
}

bool AcuriteDecoder::tryDecode(AcuriteReading * reading) {
    if (m_state == State::DATA && m_bit_count == MSG_BITS) {
        return decodeBytes(reading);
    }
    if (m_state != State::IDLE) {
        lprintf(TAG, "RESET from %s after %d bits",
            m_state == State::PREAMBLE ? "PREAMBLE" : "DATA", m_bit_count);
    }
    return false;
}

bool AcuriteDecoder::processEvent(const PulseEvent & ev, AcuriteReading * reading) {
    if (m_last_edge_us == 0) {
        m_last_edge_us = ev.timestamp_us;
        return false;
    }

    int64_t pulse_us = ev.timestamp_us - m_last_edge_us;
    m_last_edge_us = ev.timestamp_us;

    if (pulse_us > RESET_THRESHOLD_US) {
        bool ok = tryDecode(reading);
        reset();
        return ok;
    }

    // GDO0 is inverted: LOW when carrier present, HIGH when idle.
    // RISE edges (LOW→HIGH = carrier ends) carry the carrier-ON duration in pulse_us.
    if (ev.level != 1) {
        return false;
    }

    switch (m_state) {
    case State::IDLE:
        if (pulse_us >= PREAMBLE_IDLE_MIN_US && pulse_us < PREAMBLE_MAX_US) {
            m_state = State::PREAMBLE;
            m_preamble_count = 1;
            m_preamble_sum_us = pulse_us;
        }
        break;

    case State::PREAMBLE:
        if (pulse_us < NOISE_FLOOR_US) {
            // Sub-120 µs glitch — ignore, preserve preamble count.
            break;
        }
        if (pulse_us >= PREAMBLE_MAX_US) {
            // Pulse too long to be preamble — noise or stale data bit. Reset.
            reset();
        } else if (pulse_us >= PREAMBLE_IDLE_MIN_US) {
            m_preamble_count++;
            m_preamble_sum_us += pulse_us;
        } else if (m_preamble_count >= MIN_PREAMBLE_COUNT) {
            // First pulse below threshold — compute adaptive thresholds and
            // enter DATA, treating this pulse as the first data bit.
            int64_t avg = m_preamble_sum_us / m_preamble_count;
            m_data_threshold_us = avg * PREAMBLE_DATA_THRESHOLD_PCT / 100;

            lprintf(TAG,
                "PREAMBLE->DATA: preamble_avg=%lld us  data_threshold=%lld  first_bit=%lld us",
                avg, m_data_threshold_us, pulse_us);

            m_state = State::DATA;
            m_bit_count = 0;

            int bit = (pulse_us < m_data_threshold_us) ? 1 : 0;
            m_bytes[0] = (uint8_t)(bit << 7);
            m_bit_count = 1;
        } else {
            reset();
        }
        break;

    case State::DATA: {
        if (pulse_us < NOISE_FLOOR_US) {
            break;  // sub-120 µs glitch — skip without advancing bit count
        }

        int bit = (pulse_us < m_data_threshold_us) ? 1 : 0;
        int byte_idx = m_bit_count / 8;
        int bit_idx = 7 - (m_bit_count % 8);
        m_bytes[byte_idx] = (uint8_t)((m_bytes[byte_idx] & ~(1 << bit_idx)) | (bit << bit_idx));
        m_bit_count++;

        ESP_LOGD(TAG, "  bit[%2d] %4lld us -> %d", m_bit_count - 1, pulse_us, bit);

        if (m_bit_count == MSG_BITS) {
            if (decodeBytes(reading)) {
                reset();
                return true;
            }
            reset();
        }
        break;
    }
    }

    return false;
}

bool AcuriteDecoder::decodeBytes(AcuriteReading * reading) {
    // 7-byte AcuRite 592TXR packet layout (short pulse=0, long pulse=1):
    //   byte 0: [0][bat_low][ch1][ch0][id5][id4][id3][id2]
    //   byte 1: sensor ID low byte
    //   byte 2: sequence / status
    //   byte 3: humidity %
    //   byte 4: temperature high  (bits [3:0] = temp[10:7])
    //   byte 5: temperature low   (bits [6:0] = temp[6:0])
    //   byte 6: checksum = sum(bytes 0..5) & 0xFF

    uint8_t checksum = 0;
    for (int i = 0; i < 6; i++) {
        checksum += m_bytes[i];
    }

    lprintf(TAG, "decode: [%02X %02X %02X %02X %02X %02X %02X] calc=%02X",
        m_bytes[0], m_bytes[1], m_bytes[2], m_bytes[3],
        m_bytes[4], m_bytes[5], m_bytes[6], checksum);

    if (checksum != m_bytes[6]) {
        lprintf(TAG, "checksum FAIL (calc 0x%02X != stored 0x%02X)", checksum, m_bytes[6]);
        return false;
    }

    reading->sensor_id = (uint16_t)(((m_bytes[0] & 0x3F) << 8) | m_bytes[1]);
    reading->battery_ok = (m_bytes[0] & 0x40) != 0;

    uint8_t ch_bits = (m_bytes[0] >> 4) & 0x03;
    reading->channel = (ch_bits == 0x03) ? 'A' : (ch_bits == 0x02) ? 'B' : 'C';

    reading->humidity = m_bytes[3] & 0x7F;
    if (reading->humidity > 99) {
        lprintf(TAG, "sanity FAIL: humidity %d%% out of range", reading->humidity);
        return false;
    }

    // Temperature is encoded in tenths of degrees Celsius, offset by -100°C.
    int temp_raw = ((m_bytes[4] & 0x0F) << 7) | (m_bytes[5] & 0x7F);
    reading->temp_c = temp_raw / 10.0f - 100.0f;
    reading->temp_f = reading->temp_c * 9.0f / 5.0f + 32.0f;
    if (reading->temp_c < -40.0f || reading->temp_c > 60.0f) {
        lprintf(TAG, "sanity FAIL: temp %.1f C out of range", reading->temp_c);
        return false;
    }

    return true;
}
