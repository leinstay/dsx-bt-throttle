// Core of the DSX BT throttle: deduplication + rate limiting of DualSense
// Bluetooth output reports. Platform-independent so it can be unit-tested.
#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace dsxt {

// ---- CRC32 (zlib polynomial, as used by DualSense BT reports) --------------
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t len);  // crc is the running (non-inverted) state; start with 0xFFFFFFFF
inline uint32_t crc32(const uint8_t* data, size_t len) { return crc32_update(0xFFFFFFFFu, data, len) ^ 0xFFFFFFFFu; }

// DualSense BT output report on the wire: 78 bytes,
// [0]=0x31, [1]=seq<<4|tag_lo, [2]=0x10, [3..73]=payload, [74..77]=CRC32 LE.
constexpr size_t   kBtOutputReportLen = 78;
constexpr uint8_t  kBtOutputReportId  = 0x31;
constexpr uint8_t  kBtCrcSeed         = 0xA2;  // "output report" seed byte prepended before CRC

// What the Windows HID stack actually sees: WriteFile buffer sized to the HID
// descriptor's largest output report (547 bytes). Bytes 78..546 are zero padding.
constexpr size_t   kHidWriteBufLen    = 547;

bool     is_bt_output_report(const uint8_t* buf, size_t len);
uint32_t bt_report_crc(const uint8_t* buf);           // CRC over seed + buf[0..74)
bool     bt_report_crc_ok(const uint8_t* buf);
void     bt_report_finalize(uint8_t* buf, uint8_t seq); // writes seq nibble + CRC

// ---- Config / stats ---------------------------------------------------------
struct ThrottleConfig {
    bool     enabled         = true;   // false = pure passthrough (statistics only)
    bool     dedup           = true;   // drop reports identical to the last one sent
    bool     rewrite_seq_crc = true;   // renumber seq and recompute CRC on forwarded reports
    uint64_t min_interval_us = 16000;  // minimal spacing between forwarded reports
    uint64_t dup_delay_us    = 0;      // how long a dropped (duplicate) write blocks the caller
};

struct ThrottleStats {
    uint64_t calls          = 0;  // BT output writes received from the app
    uint64_t sent           = 0;  // actually forwarded to the controller
    uint64_t dropped_dup    = 0;  // dropped as duplicates
    uint64_t delayed        = 0;  // forwarded after waiting for the interval
    uint64_t wait_total_us  = 0;  // total time callers were blocked (both dup and delayed)
    uint64_t gap_total_us   = 0;  // sum of gaps between consecutive forwarded reports
    uint64_t passthrough    = 0;  // writes that were not BT output reports (forwarded untouched)
    uint64_t write_errors   = 0;
    uint64_t crc_mismatch   = 0;  // incoming reports whose CRC did not verify
};

// Abstract clock so tests can run without real time.
struct IClock {
    virtual ~IClock() = default;
    virtual uint64_t now_us() = 0;
    virtual void sleep_until_us(uint64_t t_us) = 0;
};

enum class Decision {
    Passthrough,  // not a BT output report: forward untouched, do NOT call end_write
    Forward,      // forward buf (possibly modified), then call end_write(ok)
    Drop,         // do not forward; fake success to the caller
};

// One instance per HID handle. Serializes writes on that handle.
class DeviceThrottle {
public:
    DeviceThrottle(const ThrottleConfig* cfg, IClock* clock);

    // May block (via clock->sleep_until_us). On Forward the internal I/O lock
    // stays held until end_write() is called from the same thread.
    Decision begin_write(uint8_t* buf, size_t len);
    void end_write(bool ok);

    ThrottleStats stats() const;
    bool crc_verified() const { return m_crc_checked && m_crc_ok; }

private:
    const ThrottleConfig* m_cfg;
    IClock*               m_clock;

    std::mutex            m_io;         // held across begin_write..end_write for forwarded reports
    mutable std::mutex    m_stats_mx;
    ThrottleStats         m_stats;

    std::vector<uint8_t>  m_last_key;   // payload of the last forwarded report (without seq/CRC)
    bool                  m_have_last    = false;
    uint64_t              m_last_sent_us = 0;
    uint64_t              m_last_ret_us  = 0;  // when the previous begin_write on this device returned
    bool                  m_ever_sent    = false;
    uint8_t               m_seq          = 0;
    bool                  m_crc_checked  = false;
    bool                  m_crc_ok       = false;
    bool                  m_rewrite      = true;  // may be auto-disabled if incoming CRC does not verify
    uint64_t              m_pending_wait = 0;
};

}  // namespace dsxt
