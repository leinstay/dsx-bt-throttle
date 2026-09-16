#include "config.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace dsxt {
namespace {
const wchar_t* kSec = L"throttle";

int geti(const std::wstring& ini, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(kSec, key, def, ini.c_str());
}
std::wstring gets(const std::wstring& ini, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(kSec, key, def, buf, 1024, ini.c_str());
    return buf;
}
int clamp_ms(int v, int def) {
    if (v < 0) return def;
    return v > 1000 ? 1000 : v;
}
}  // namespace

AppConfig load_config(const std::wstring& dll_dir) {
    AppConfig c;
    c.ini_path = dll_dir + L"\\dsx_throttle.ini";
    const std::wstring& ini = c.ini_path;

    c.throttle.enabled         = geti(ini, L"enabled", 1) != 0;
    c.throttle.dedup           = geti(ini, L"dedup", 1) != 0;
    c.throttle.rewrite_seq_crc = geti(ini, L"rewrite_seq_crc", 1) != 0;
    int interval_ms            = clamp_ms(geti(ini, L"min_interval_ms", 16), 16);
    c.throttle.min_interval_us = (uint64_t)interval_ms * 1000;
    int dup_ms                 = geti(ini, L"dup_delay_ms", 0);
    if (dup_ms < 0) dup_ms = interval_ms;   // -1 = same as interval
    c.throttle.dup_delay_us    = (uint64_t)clamp_ms(dup_ms, 0) * 1000;

    c.trace            = geti(ini, L"trace", 0) != 0;
    c.trace_max_lines  = (unsigned)geti(ini, L"trace_max_lines", 3000);
    c.stats_interval_s = (unsigned)geti(ini, L"stats_interval_s", 10);
    c.log_enabled      = geti(ini, L"log", 1) != 0;
    c.log_append       = geti(ini, L"log_append", 0) != 0;

    std::wstring lp = gets(ini, L"log_path", L"dsx_throttle.log");
    if (lp.size() > 1 && (lp[1] == L':' || lp[0] == L'\\')) c.log_path = lp;
    else c.log_path = dll_dir + L"\\" + lp;
    return c;
}

}  // namespace dsxt
