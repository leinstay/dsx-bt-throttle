// Win32 side of the throttle: MinHook hooks on kernelbase!WriteFile and
// kernelbase!CloseHandle inside DSX.exe.
//
// DualSense output reports are recognised by content, not by handle: DSX
// opens the HID device before we are loaded, so a CreateFileW hook would miss
// it. Every WriteFile whose buffer is exactly kHidWriteBufLen bytes and starts
// with report id 0x31 is a Bluetooth output report; the first 78 bytes are the
// wire-format packet and the rest is zero padding required by the HID stack.
#include "hooks.h"

#include <timeapi.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "MinHook.h"
#include "config.h"
#include "log.h"
#include "throttle.h"
#include "version.h"

namespace dsxt {
namespace {

// ------------------------------------------------------------------ clock ---
class QpcClock : public IClock {
public:
    QpcClock() { QueryPerformanceFrequency(&m_freq); }

    uint64_t now_us() override {
        LARGE_INTEGER c; QueryPerformanceCounter(&c);
        return (uint64_t)((c.QuadPart * 1000000.0) / (double)m_freq.QuadPart);
    }

    void sleep_until_us(uint64_t t) override {
        HANDLE timer = thread_timer();
        for (;;) {
            uint64_t now = now_us();
            if (now >= t) return;
            uint64_t rem = t - now;
            if (timer && rem > 300) {
                LARGE_INTEGER due; due.QuadPart = -(LONGLONG)(rem * 10);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                    WaitForSingleObject(timer, (DWORD)(rem / 1000 + 2));
                    continue;
                }
            }
            if (rem > 1500) Sleep(1); else SwitchToThread();
        }
    }

    bool hires() const { return m_hires; }

private:
    LARGE_INTEGER m_freq{};
    bool          m_hires = true;

    HANDLE thread_timer() {
        static thread_local HANDLE h = [this]() -> HANDLE {
            HANDLE t = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
            if (!t) { t = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS); m_hires = false; }
            return t;
        }();
        return h;
    }
};

// ---------------------------------------------------------------- state -----
struct Device {
    Device(const ThrottleConfig* c, IClock* k) : thr(c, k) {}
    DeviceThrottle thr;
    ThrottleStats  last_stats;
};

AppConfig g_cfg;
QpcClock  g_clock;
HMODULE   g_self = nullptr;

// Cleared by stop(). Checked before anything else in the hooks so that calls
// arriving between DLL_PROCESS_DETACH and the actual unhook touch no state.
std::atomic<bool>                                   g_active{false};
std::shared_mutex                                   g_devices_mx;
std::unordered_map<HANDLE, std::shared_ptr<Device>> g_devices;
std::atomic<uint64_t>                               g_write_calls{0};

// Our own logging goes through WriteFile too; the guard keeps the hook from
// re-entering itself on that path.
thread_local int t_depth = 0;
struct HookGuard {
    HookGuard()  { ++t_depth; }
    ~HookGuard() { --t_depth; }
    static bool nested() { return t_depth > 0; }
};

std::shared_ptr<Device> find_device(HANDLE h) {
    std::shared_lock<std::shared_mutex> g(g_devices_mx);
    auto it = g_devices.find(h);
    return it == g_devices.end() ? nullptr : it->second;
}

std::shared_ptr<Device> get_or_add_device(HANDLE h, const uint8_t* buf) {
    if (auto d = find_device(h)) return d;
    std::unique_lock<std::shared_mutex> g(g_devices_mx);
    auto& slot = g_devices[h];
    if (!slot) {
        slot = std::make_shared<Device>(&g_cfg.throttle, &g_clock);
        Log::info("DualSense output detected on h=%p (report id 0x%02X, seq/tag 0x%02X, flags 0x%02X)",
                  (void*)h, buf[0], buf[1], buf[2]);
    }
    return slot;
}

std::shared_ptr<Device> remove_device(HANDLE h) {
    std::unique_lock<std::shared_mutex> g(g_devices_mx);
    auto it = g_devices.find(h);
    if (it == g_devices.end()) return nullptr;
    auto d = it->second;
    g_devices.erase(it);
    return d;
}

inline bool is_ds_hid_write(const uint8_t* buf, DWORD len) {
    return len == kHidWriteBufLen && buf[0] == kBtOutputReportId;
}

// ---------------------------------------------------------------- hooks -----
typedef BOOL (WINAPI *WriteFile_fn)(HANDLE, LPCVOID, DWORD, LPDWORD, LPOVERLAPPED);
typedef BOOL (WINAPI *CloseHandle_fn)(HANDLE);

WriteFile_fn   Real_WriteFile   = nullptr;
CloseHandle_fn Real_CloseHandle = nullptr;

BOOL WINAPI Hook_WriteFile(HANDLE hFile, LPCVOID buf, DWORD nBytes, LPDWORD written, LPOVERLAPPED ov) {
    if (!g_active.load(std::memory_order_relaxed) || !buf || !is_ds_hid_write((const uint8_t*)buf, nBytes) || HookGuard::nested())
        return Real_WriteFile(hFile, buf, nBytes, written, ov);

    HookGuard guard;
    g_write_calls.fetch_add(1, std::memory_order_relaxed);

    uint8_t* report = (uint8_t*)buf;  // DSX owns the buffer; seq/CRC rewrite is in place
    auto dev = get_or_add_device(hFile, report);

    switch (dev->thr.begin_write(report, kBtOutputReportLen)) {
    case Decision::Passthrough:
        return Real_WriteFile(hFile, buf, nBytes, written, ov);

    case Decision::Drop:
        Log::trace("h=%p DROP dup", (void*)hFile);
        if (written) *written = nBytes;
        if (ov) {
            ov->Internal = 0;
            ov->InternalHigh = nBytes;
            if (ov->hEvent) SetEvent(ov->hEvent);
        }
        SetLastError(ERROR_SUCCESS);
        return TRUE;

    case Decision::Forward: {
        BOOL r = Real_WriteFile(hFile, buf, nBytes, written, ov);
        bool ok = r || GetLastError() == ERROR_IO_PENDING;
        dev->thr.end_write(ok);
        Log::trace("h=%p SENT seq=%u ok=%d", (void*)hFile, report[1] >> 4, (int)ok);
        return r;
    }
    }
    return Real_WriteFile(hFile, buf, nBytes, written, ov);
}

BOOL WINAPI Hook_CloseHandle(HANDLE h) {
    if (g_active.load(std::memory_order_relaxed) && !HookGuard::nested()) {
        if (auto dev = remove_device(h)) {
            HookGuard guard;
            ThrottleStats s = dev->thr.stats();
            Log::info("h=%p closed: in=%llu out=%llu dup=%llu",
                      (void*)h, (unsigned long long)s.calls, (unsigned long long)s.sent, (unsigned long long)s.dropped_dup);
        }
    }
    return Real_CloseHandle(h);
}

// ---------------------------------------------------------------- stats -----
void log_stats(HANDLE h, Device& d) {
    ThrottleStats s = d.thr.stats();
    const ThrottleStats& p = d.last_stats;
    uint64_t dcalls = s.calls - p.calls, dsent = s.sent - p.sent;
    if (dcalls == 0 && s.passthrough == p.passthrough) return;

    double secs    = (double)g_cfg.stats_interval_s;
    double avg_gap = s.sent > 1 ? (double)s.gap_total_us / (double)(s.sent - 1) / 1000.0 : 0.0;
    Log::info("stats h=%p: in=%llu (%.0f/s) out=%llu (%.0f/s) dup=%llu delayed=%llu err=%llu crc_fail=%llu | avg gap %.1f ms | blocked %.2f s",
              (void*)h, (unsigned long long)s.calls, dcalls / secs, (unsigned long long)s.sent, dsent / secs,
              (unsigned long long)s.dropped_dup, (unsigned long long)s.delayed, (unsigned long long)s.write_errors,
              (unsigned long long)s.crc_mismatch, avg_gap, s.wait_total_us / 1e6);
    d.last_stats = s;
}

DWORD WINAPI stats_thread(LPVOID) {
    unsigned tick = 0;
    for (;;) {
        Sleep(g_cfg.stats_interval_s * 1000);
        ++tick;

        std::vector<std::pair<HANDLE, std::shared_ptr<Device>>> snap;
        {
            std::shared_lock<std::shared_mutex> g(g_devices_mx);
            snap.assign(g_devices.begin(), g_devices.end());
        }
        if (snap.empty()) {
            if (tick <= 3 || tick % 6 == 0)
                Log::info("waiting for DualSense output (matching writes so far: %llu)", (unsigned long long)g_write_calls.load());
            continue;
        }
        for (auto& kv : snap) log_stats(kv.first, *kv.second);
    }
}

// ----------------------------------------------------------------- init -----
std::wstring module_dir(HMODULE m) {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(m, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    std::wstring p(buf, n);
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : p.substr(0, k);
}

template <typename T>
bool hook(const wchar_t* mod, const char* name, T detour, T* real) {
    HMODULE h = GetModuleHandleW(mod);
    FARPROC fn = h ? GetProcAddress(h, name) : nullptr;
    if (!fn) { Log::warn("%s!%s not found", Log::narrow(mod).c_str(), name); return false; }
    MH_STATUS st = MH_CreateHook((LPVOID)fn, (LPVOID)detour, (LPVOID*)real);
    if (st != MH_OK) { Log::warn("hook %s!%s failed: %s", Log::narrow(mod).c_str(), name, MH_StatusToString(st)); return false; }
    Log::info("hooked %s!%s at %p", Log::narrow(mod).c_str(), name, (void*)fn);
    return true;
}

DWORD WINAPI init_thread(LPVOID) {
    std::wstring dir = module_dir(g_self);
    g_cfg = load_config(dir);
    if (g_cfg.log_enabled) Log::open(g_cfg.log_path, !g_cfg.log_append);
    Log::set_trace(g_cfg.trace, g_cfg.trace_max_lines);

    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    Log::info("dsx_throttle " DSXT_VERSION " in pid %lu (%s)", GetCurrentProcessId(), Log::narrow(exe).c_str());
    Log::info("config: enabled=%d dedup=%d rewrite_seq_crc=%d min_interval=%llu ms dup_delay=%llu ms trace=%d stats_every=%u s",
              (int)g_cfg.throttle.enabled, (int)g_cfg.throttle.dedup, (int)g_cfg.throttle.rewrite_seq_crc,
              (unsigned long long)(g_cfg.throttle.min_interval_us / 1000), (unsigned long long)(g_cfg.throttle.dup_delay_us / 1000),
              (int)g_cfg.trace, g_cfg.stats_interval_s);
    if (GetFileAttributesW(g_cfg.ini_path.c_str()) == INVALID_FILE_ATTRIBUTES)
        Log::warn("%s not found, using defaults", Log::narrow(g_cfg.ini_path).c_str());

    MH_STATUS st = MH_Initialize();
    if (st != MH_OK) { Log::warn("MH_Initialize failed: %s", MH_StatusToString(st)); return 1; }

    // kernel32!WriteFile is a forwarder on modern Windows; hooking kernelbase
    // catches every caller exactly once.
    bool ok = hook(L"kernelbase.dll", "WriteFile",   &Hook_WriteFile,   &Real_WriteFile);
    ok     &= hook(L"kernelbase.dll", "CloseHandle", &Hook_CloseHandle, &Real_CloseHandle);
    if (!ok) return 1;

    g_active.store(true);
    st = MH_EnableHook(MH_ALL_HOOKS);
    if (st != MH_OK) { g_active.store(false); Log::warn("MH_EnableHook failed: %s", MH_StatusToString(st)); return 1; }
    Log::info("hooks enabled");

    g_clock.sleep_until_us(g_clock.now_us() + 500);  // forces timer creation on this thread
    if (!g_clock.hires()) { timeBeginPeriod(1); Log::warn("high-resolution timers unavailable, using timeBeginPeriod(1)"); }

    if (g_cfg.stats_interval_s) {
        HANDLE t = CreateThread(nullptr, 0, stats_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return 0;
}

}  // namespace

void start(HMODULE self) {
    g_self = self;
    HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
    if (t) CloseHandle(t);
}

void stop() {
    if (!g_active.exchange(false)) return;
    Log::info("unloading, restoring original functions");
    MH_DisableHook(MH_ALL_HOOKS);
    MH_Uninitialize();
    Log::close();
}

}  // namespace dsxt
