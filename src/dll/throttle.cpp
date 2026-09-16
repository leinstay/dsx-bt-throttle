#include "throttle.h"

#include <algorithm>
#include <cstring>

namespace dsxt {

// ---------------------------------------------------------------- CRC32 -----
namespace {
struct CrcTable {
    uint32_t t[256];
    CrcTable() {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
    }
};
const CrcTable& table() { static CrcTable tb; return tb; }
}  // namespace

uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len) {
    const CrcTable& tb = table();
    for (size_t i = 0; i < len; ++i) crc = tb.t[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc;
}

bool is_bt_output_report(const uint8_t* buf, size_t len) {
    return buf && len == kBtOutputReportLen && buf[0] == kBtOutputReportId;
}

uint32_t bt_report_crc(const uint8_t* buf) {
    uint32_t c = crc32_update(0xFFFFFFFFu, &kBtCrcSeed, 1);
    c = crc32_update(c, buf, kBtOutputReportLen - 4);
    return c ^ 0xFFFFFFFFu;
}

bool bt_report_crc_ok(const uint8_t* buf) {
    const size_t o = kBtOutputReportLen - 4;
    uint32_t stored = (uint32_t)buf[o] | ((uint32_t)buf[o + 1] << 8) | ((uint32_t)buf[o + 2] << 16) | ((uint32_t)buf[o + 3] << 24);
    return stored == bt_report_crc(buf);
}

void bt_report_finalize(uint8_t* buf, uint8_t seq) {
    buf[1] = (uint8_t)((seq << 4) | (buf[1] & 0x0F));
    uint32_t c = bt_report_crc(buf);
    const size_t o = kBtOutputReportLen - 4;
    buf[o] = (uint8_t)c; buf[o + 1] = (uint8_t)(c >> 8); buf[o + 2] = (uint8_t)(c >> 16); buf[o + 3] = (uint8_t)(c >> 24);
}

// ------------------------------------------------------- DeviceThrottle -----
DeviceThrottle::DeviceThrottle(const ThrottleConfig* cfg, IClock* clock) : m_cfg(cfg), m_clock(clock) {}

ThrottleStats DeviceThrottle::stats() const {
    std::lock_guard<std::mutex> g(m_stats_mx);
    return m_stats;
}

Decision DeviceThrottle::begin_write(uint8_t* buf, size_t len) {
    if (!is_bt_output_report(buf, len)) {
        std::lock_guard<std::mutex> g(m_stats_mx);
        ++m_stats.passthrough;
        return Decision::Passthrough;
    }

    m_io.lock();  // released in end_write() for Forward, below for Drop
    {
        std::lock_guard<std::mutex> g(m_stats_mx);
        ++m_stats.calls;
    }

    // One-time self check: does our CRC agree with what the app produces?
    if (!m_crc_checked) {
        m_crc_checked = true;
        m_crc_ok = bt_report_crc_ok(buf);
        if (!m_crc_ok) m_rewrite = false;  // never touch bytes we do not fully understand
    } else if (!bt_report_crc_ok(buf)) {
        std::lock_guard<std::mutex> g(m_stats_mx);
        ++m_stats.crc_mismatch;
    }

    if (!m_cfg->enabled) {
        m_pending_wait = 0;
        return Decision::Forward;  // passthrough, but still measures the native send rate
    }

    const uint8_t* key_begin = buf + 2;
    const size_t   key_len   = kBtOutputReportLen - 2 - 4;
    const uint64_t now       = m_clock->now_us();

    const bool dup = m_cfg->dedup && m_have_last && m_last_key.size() == key_len &&
                     std::memcmp(m_last_key.data(), key_begin, key_len) == 0;

    if (dup) {
        // Optionally pace the caller so a tight writer loop cannot spin at 100% CPU.
        uint64_t wake = std::max(m_last_sent_us + m_cfg->min_interval_us, m_last_ret_us + m_cfg->dup_delay_us);
        uint64_t waited = 0;
        if (m_cfg->dup_delay_us && wake > now) {
            m_clock->sleep_until_us(wake);
            waited = wake - now;
        }
        m_last_ret_us = m_clock->now_us();
        {
            std::lock_guard<std::mutex> g(m_stats_mx);
            ++m_stats.dropped_dup;
            m_stats.wait_total_us += waited;
        }
        m_io.unlock();
        return Decision::Drop;
    }

    // Changed report: enforce minimal spacing between real sends.
    m_pending_wait = 0;
    if (m_ever_sent) {
        uint64_t next_ok = m_last_sent_us + m_cfg->min_interval_us;
        if (next_ok > now) {
            m_clock->sleep_until_us(next_ok);
            m_pending_wait = next_ok - now;
        }
    }

    if (m_cfg->rewrite_seq_crc && m_rewrite) {
        bt_report_finalize(buf, m_seq);
        m_seq = (uint8_t)((m_seq + 1) & 0x0F);
    }
    m_last_key.assign(key_begin, key_begin + key_len);
    return Decision::Forward;
}

void DeviceThrottle::end_write(bool ok) {
    const uint64_t now = m_clock->now_us();
    {
        std::lock_guard<std::mutex> g(m_stats_mx);
        if (ok) {
            ++m_stats.sent;
            if (m_ever_sent) m_stats.gap_total_us += now - m_last_sent_us;
            if (m_pending_wait) { ++m_stats.delayed; m_stats.wait_total_us += m_pending_wait; }
        } else {
            ++m_stats.write_errors;
        }
    }
    if (ok) {
        m_have_last    = true;
        m_ever_sent    = true;
        m_last_sent_us = now;
    } else {
        m_have_last = false;  // retry the same content next time
    }
    m_last_ret_us  = now;
    m_pending_wait = 0;
    m_io.unlock();
}

}  // namespace dsxt
