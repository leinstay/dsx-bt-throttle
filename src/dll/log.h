#pragma once
#include <string>

namespace dsxt {

// Tiny thread-safe file logger. Lines are timestamped and flushed immediately.
class Log {
public:
    static void open(const std::wstring& path, bool truncate);
    static void close();
    static bool enabled();
    static void info(const char* fmt, ...);
    static void warn(const char* fmt, ...);
    static void trace(const char* fmt, ...);   // only written when trace mode is on
    static void set_trace(bool on, unsigned max_lines);
    static std::string narrow(const std::wstring& s);
};

}  // namespace dsxt
