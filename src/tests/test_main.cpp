// Unit tests for the throttle core. Minimal harness, no external deps.
#include "../dll/throttle.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace dsxt;

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)
#define CHECK_EQ(a, b) do { auto _a = (a); auto _b = (b); if (_a == _b) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s == %s  (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b, (unsigned long long)_a, (unsigned long long)_b); } } while (0)

struct FakeClock : IClock {
    uint64_t t = 0;
    uint64_t total_slept = 0;
    uint64_t now_us() override { return t; }
    void sleep_until_us(uint64_t tt) override { if (tt > t) { total_slept += tt - t; t = tt; } }
};

static std::vector<uint8_t> make_report(uint8_t rumble_l, uint8_t seq = 0) {
    std::vector<uint8_t> r(kBtOutputReportLen, 0);
    r[0] = 0x31; r[1] = (uint8_t)(seq << 4); r[2] = 0x10;
    r[3] = 0xFF; r[4] = 0xF7;     // enable flags
    r[5] = rumble_l; r[6] = 0x00; // rumble
    bt_report_finalize(r.data(), seq);
    return r;
}

static void test_crc() {
    const char* s = "123456789";
    CHECK_EQ(crc32((const uint8_t*)s, 9), 0xCBF43926u);
    // Seed byte semantics: crc(seed + data) == bt_report_crc(data)
    std::vector<uint8_t> r(kBtOutputReportLen, 0x5A);
    r[0] = 0x31;
    std::vector<uint8_t> seeded; seeded.push_back(kBtCrcSeed); seeded.insert(seeded.end(), r.begin(), r.begin() + 74);
    CHECK_EQ(bt_report_crc(r.data()), crc32(seeded.data(), seeded.size()));
    // finalize -> verify ok; corrupt -> not ok
    bt_report_finalize(r.data(), 5);
    CHECK(bt_report_crc_ok(r.data()));
    CHECK_EQ(r[1] >> 4, 5u);
    r[10] ^= 1;
    CHECK(!bt_report_crc_ok(r.data()));
    // seq preserves low nibble
    r[1] = 0x03; bt_report_finalize(r.data(), 0xF);
    CHECK_EQ(r[1], 0xF3u);
}

static void test_recognize() {
    auto r = make_report(1);
    CHECK(is_bt_output_report(r.data(), r.size()));
    CHECK(!is_bt_output_report(r.data(), 64));
    uint8_t usb[48] = { 0x02 };
    CHECK(!is_bt_output_report(usb, 48));
}

static void test_dedup_and_rate() {
    ThrottleConfig cfg; cfg.min_interval_us = 16000; cfg.dup_delay_us = 16000;
    FakeClock clk;
    DeviceThrottle d(&cfg, &clk);

    // 1st report: forwarded immediately, no wait
    auto a = make_report(10, 3);
    CHECK(d.begin_write(a.data(), a.size()) == Decision::Forward);
    CHECK_EQ(a[1] >> 4, 0u);            // renumbered from 0
    CHECK(bt_report_crc_ok(a.data()));
    d.end_write(true);
    CHECK_EQ(clk.total_slept, 0u);

    // Same content, different app seq -> dropped, caller paced to next slot (t=16ms)
    clk.t = 1000;
    auto a2 = make_report(10, 4);
    CHECK(d.begin_write(a2.data(), a2.size()) == Decision::Drop);
    CHECK_EQ(clk.t, 16000u);

    // Another dup right away -> paced by dup_delay from last return
    auto a3 = make_report(10, 5);
    CHECK(d.begin_write(a3.data(), a3.size()) == Decision::Drop);
    CHECK_EQ(clk.t, 32000u);

    // Changed content at t=35ms: last send was at t=0 -> allowed immediately
    clk.t = 35000;
    auto b = make_report(20, 6);
    CHECK(d.begin_write(b.data(), b.size()) == Decision::Forward);
    CHECK_EQ(clk.t, 35000u);
    CHECK_EQ(b[1] >> 4, 1u);
    d.end_write(true);

    // Changed content 5ms later -> delayed until 35+16=51ms
    clk.t = 40000;
    auto c = make_report(30, 7);
    CHECK(d.begin_write(c.data(), c.size()) == Decision::Forward);
    CHECK_EQ(clk.t, 51000u);
    d.end_write(true);

    auto s = d.stats();
    CHECK_EQ(s.calls, 5u);
    CHECK_EQ(s.sent, 3u);
    CHECK_EQ(s.dropped_dup, 2u);
    CHECK_EQ(s.delayed, 1u);
    CHECK_EQ(s.gap_total_us, 51000u);
    CHECK_EQ(s.crc_mismatch, 0u);
    CHECK(d.crc_verified());
}

static void test_dup_without_delay_returns_instantly() {
    ThrottleConfig cfg; cfg.min_interval_us = 16000; cfg.dup_delay_us = 0;
    FakeClock clk;
    DeviceThrottle d(&cfg, &clk);
    auto a = make_report(10);
    CHECK(d.begin_write(a.data(), a.size()) == Decision::Forward);
    d.end_write(true);
    clk.t = 1000;
    for (int i = 0; i < 50; ++i) {
        auto dup = make_report(10, (uint8_t)i);
        CHECK(d.begin_write(dup.data(), dup.size()) == Decision::Drop);
    }
    CHECK_EQ(clk.t, 1000u);             // caller was never blocked
    CHECK_EQ(clk.total_slept, 0u);
    CHECK_EQ(d.stats().dropped_dup, 50u);
}

static void test_seq_wraps() {
    ThrottleConfig cfg; cfg.min_interval_us = 0; cfg.dup_delay_us = 0;
    FakeClock clk;
    DeviceThrottle d(&cfg, &clk);
    for (int i = 0; i < 20; ++i) {
        auto r = make_report((uint8_t)i);
        CHECK(d.begin_write(r.data(), r.size()) == Decision::Forward);
        CHECK_EQ(r[1] >> 4, (unsigned)(i & 0xF));
        d.end_write(true);
    }
}

static void test_disabled_passthrough() {
    ThrottleConfig cfg; cfg.enabled = false;
    FakeClock clk;
    DeviceThrottle d(&cfg, &clk);
    auto a = make_report(1, 9);
    auto copy = a;
    CHECK(d.begin_write(a.data(), a.size()) == Decision::Forward);
    CHECK(a == copy);                 // untouched
    d.end_write(true);
    clk.t = 100;
    auto b = make_report(1, 10);      // dup content, but disabled -> still forwarded, no wait
    CHECK(d.begin_write(b.data(), b.size()) == Decision::Forward);
    CHECK_EQ(clk.t, 100u);
    d.end_write(true);
    CHECK_EQ(d.stats().sent, 2u);
}

static void test_non_bt_passthrough() {
    ThrottleConfig cfg;
    FakeClock clk;
    DeviceThrottle d(&cfg, &clk);
    uint8_t usb[48] = { 0x02, 0xFF };
    CHECK(d.begin_write(usb, sizeof usb) == Decision::Passthrough);
    CHECK(d.begin_write(usb, sizeof usb) == Decision::Passthrough);  // never deduped / delayed
    CHECK_EQ(clk.t, 0u);
    CHECK_EQ(d.stats().passthrough, 2u);
}

static void test_write_error_retries() {
    ThrottleConfig cfg; cfg.min_interval_us = 0; cfg.dup_delay_us = 0;
    FakeClock clk;
    DeviceThrottle d(&cfg, &clk);
    auto a = make_report(7);
    CHECK(d.begin_write(a.data(), a.size()) == Decision::Forward);
    d.end_write(false);
    auto b = make_report(7);
    CHECK(d.begin_write(b.data(), b.size()) == Decision::Forward);  // not treated as dup after a failure
    d.end_write(true);
    CHECK_EQ(d.stats().write_errors, 1u);
}

static void test_bad_crc_disables_rewrite() {
    ThrottleConfig cfg; cfg.min_interval_us = 0; cfg.dup_delay_us = 0;
    FakeClock clk;
    DeviceThrottle d(&cfg, &clk);
    auto a = make_report(7, 5);
    a[74] ^= 0xFF;  // app "uses a different CRC"
    auto copy = a;
    CHECK(d.begin_write(a.data(), a.size()) == Decision::Forward);
    CHECK(a == copy);  // nothing rewritten
    CHECK(!d.crc_verified());
    d.end_write(true);
}

static void test_hid_buffer_prefix() {
    // The hook hands the throttle only the first 78 bytes of the 547-byte HID buffer.
    std::vector<uint8_t> hid(kHidWriteBufLen, 0);
    auto wire = make_report(42, 3);
    std::memcpy(hid.data(), wire.data(), kBtOutputReportLen);

    ThrottleConfig cfg; cfg.min_interval_us = 0; cfg.dup_delay_us = 0;
    FakeClock clk;
    DeviceThrottle d(&cfg, &clk);
    CHECK(d.begin_write(hid.data(), kBtOutputReportLen) == Decision::Forward);
    CHECK_EQ(hid[1] >> 4, 0u);
    CHECK(bt_report_crc_ok(hid.data()));
    CHECK(std::count(hid.begin() + kBtOutputReportLen, hid.end(), 0) == (long long)(kHidWriteBufLen - kBtOutputReportLen));
    d.end_write(true);
}

int main() {
    test_crc();
    test_recognize();
    test_dedup_and_rate();
    test_dup_without_delay_returns_instantly();
    test_seq_wraps();
    test_disabled_passthrough();
    test_non_bt_passthrough();
    test_write_error_retries();
    test_bad_crc_disables_rewrite();
    test_hid_buffer_prefix();
    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
