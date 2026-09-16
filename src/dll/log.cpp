#include "log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>

namespace dsxt {
namespace {
FILE*      g_file = nullptr;
std::recursive_mutex g_mx;
bool       g_trace = false;
unsigned   g_trace_left = 0;

void vwrite(const char* level, const char* fmt, va_list ap) {
    std::lock_guard<std::recursive_mutex> g(g_mx);
    if (!g_file) return;
    SYSTEMTIME st; GetLocalTime(&st);
    std::fprintf(g_file, "%02u:%02u:%02u.%03u [%s] [t%lu] ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, level, GetCurrentThreadId());
    std::vfprintf(g_file, fmt, ap);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}
}  // namespace

void Log::open(const std::wstring& path, bool truncate) {
    std::lock_guard<std::recursive_mutex> g(g_mx);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
    g_file = _wfsopen(path.c_str(), truncate ? L"w" : L"a", _SH_DENYNO);
}

void Log::close() {
    std::lock_guard<std::recursive_mutex> g(g_mx);
    if (g_file) { std::fclose(g_file); g_file = nullptr; }
}

bool Log::enabled() { return g_file != nullptr; }

void Log::set_trace(bool on, unsigned max_lines) { g_trace = on; g_trace_left = max_lines; }

void Log::info(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); vwrite("INF", fmt, ap); va_end(ap); }
void Log::warn(const char* fmt, ...)  { va_list ap; va_start(ap, fmt); vwrite("WRN", fmt, ap); va_end(ap); }
void Log::trace(const char* fmt, ...) {
    if (!g_trace) return;
    if (g_trace_left == 0) { g_trace = false; info("trace limit reached, tracing stopped"); return; }
    --g_trace_left;
    va_list ap; va_start(ap, fmt); vwrite("TRC", fmt, ap); va_end(ap);
}

std::string Log::narrow(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.c_str(), (int)s.size(), &out[0], n, nullptr, nullptr);
    return out;
}

}  // namespace dsxt
