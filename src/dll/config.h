#pragma once
#include <string>
#include "throttle.h"

namespace dsxt {

struct AppConfig {
    ThrottleConfig throttle;
    bool         trace            = false;   // per-write trace lines in the log
    unsigned     trace_max_lines  = 3000;
    unsigned     stats_interval_s = 10;      // 0 = never
    bool         log_enabled      = true;
    bool         log_append       = false;
    std::wstring log_path;                   // resolved absolute path
    std::wstring ini_path;
};

// Loads <dll dir>\dsx_throttle.ini. Missing file/keys -> defaults.
AppConfig load_config(const std::wstring& dll_dir);

}  // namespace dsxt
