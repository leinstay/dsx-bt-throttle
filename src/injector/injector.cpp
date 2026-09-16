// dsx_throttle_injector.exe - console manager for dsx_throttle.dll.
// Finds DSX.exe, loads the DLL into it with CreateRemoteThread+LoadLibraryW
// and tails the log. Alternative to the version.dll drop-in for cases where
// DSX is already running.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <string>
#include <vector>

#include "../dll/version.h"

namespace {

std::wstring lower(std::wstring s) { for (auto& c : s) c = (wchar_t)towlower(c); return s; }

std::wstring exe_dir() {
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof buf / sizeof buf[0]));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/"));
}

struct Target { DWORD pid = 0; bool has_hid = false; bool has_dll = false; std::wstring path; };

std::vector<Target> find_dsx(const std::wstring& dll_name) {
    std::vector<Target> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof pe;
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (_wcsicmp(pe.szExeFile, L"DSX.exe") != 0) continue;
        Target t; t.pid = pe.th32ProcessID;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_VM_READ, FALSE, t.pid);
        if (p) {
            wchar_t path[MAX_PATH * 2]; DWORD pn = (DWORD)(sizeof path / sizeof path[0]);
            if (QueryFullProcessImageNameW(p, 0, path, &pn)) t.path.assign(path, pn);
            std::vector<HMODULE> mods(1024);
            DWORD needed = 0;
            if (EnumProcessModulesEx(p, mods.data(), (DWORD)(mods.size() * sizeof(HMODULE)), &needed, LIST_MODULES_ALL)) {
                size_t cnt = needed / sizeof(HMODULE);
                if (cnt > mods.size()) cnt = mods.size();
                for (size_t i = 0; i < cnt; ++i) {
                    wchar_t mp[MAX_PATH];
                    if (!GetModuleBaseNameW(p, mods[i], mp, MAX_PATH)) continue;
                    std::wstring b = lower(mp);
                    if (b == L"hidsharp.dll" || b == L"hidapi.net.dll") t.has_hid = true;
                    if (b == lower(dll_name)) t.has_dll = true;
                }
            }
            CloseHandle(p);
        }
        out.push_back(t);
    }
    CloseHandle(snap);
    return out;
}

bool inject(DWORD pid, const std::wstring& dll_path, std::wstring& err_out) {
    HANDLE p = OpenProcess(PROCESS_CREATE_THREAD | PROCESS_QUERY_INFORMATION | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ, FALSE, pid);
    if (!p) { err_out = L"OpenProcess failed: " + std::to_wstring(GetLastError()) + L" (run as administrator?)"; return false; }

    BOOL wow = FALSE;
    IsWow64Process(p, &wow);
    if (wow) { err_out = L"target is 32-bit, this injector is 64-bit only"; CloseHandle(p); return false; }

    SIZE_T bytes = (dll_path.size() + 1) * sizeof(wchar_t);
    LPVOID mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem) { err_out = L"VirtualAllocEx failed: " + std::to_wstring(GetLastError()); CloseHandle(p); return false; }
    if (!WriteProcessMemory(p, mem, dll_path.c_str(), bytes, nullptr)) {
        err_out = L"WriteProcessMemory failed: " + std::to_wstring(GetLastError());
        VirtualFreeEx(p, mem, 0, MEM_RELEASE); CloseHandle(p); return false;
    }
    auto loadlib = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE th = CreateRemoteThread(p, nullptr, 0, loadlib, mem, 0, nullptr);
    if (!th) {
        err_out = L"CreateRemoteThread failed: " + std::to_wstring(GetLastError());
        VirtualFreeEx(p, mem, 0, MEM_RELEASE); CloseHandle(p); return false;
    }
    DWORD w = WaitForSingleObject(th, 15000);
    DWORD code = 0;
    if (w == WAIT_OBJECT_0) GetExitCodeThread(th, &code);
    CloseHandle(th);
    VirtualFreeEx(p, mem, 0, MEM_RELEASE);
    CloseHandle(p);
    if (w != WAIT_OBJECT_0) { err_out = L"remote LoadLibraryW did not finish in time"; return false; }
    if (code == 0) { err_out = L"LoadLibraryW returned NULL (DLL failed to load - missing dep? blocked by AV?)"; return false; }
    return true;
}

// -------------------------------------------------------------- log tail -----
struct LogTail {
    std::wstring path;
    HANDLE hFile = INVALID_HANDLE_VALUE;
    LARGE_INTEGER lastSize{};
    std::deque<std::string> lines;
    static const size_t MAX_LINES = 200;

    void set_path(const std::wstring& p) { path = p; close(); }
    void close() { if (hFile != INVALID_HANDLE_VALUE) { CloseHandle(hFile); hFile = INVALID_HANDLE_VALUE; } lastSize.QuadPart = 0; }

    void poll() {
        if (path.empty()) return;
        WIN32_FILE_ATTRIBUTE_DATA attr{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attr)) return;
        LARGE_INTEGER sz; sz.LowPart = attr.nFileSizeLow; sz.HighPart = attr.nFileSizeHigh;
        if (sz.QuadPart < lastSize.QuadPart) {
            // File was truncated (new DLL injection); reset
            close();
            lines.clear();
            lastSize.QuadPart = 0;
        }
        if (sz.QuadPart <= lastSize.QuadPart) return;

        if (hFile == INVALID_HANDLE_VALUE) {
            hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
            if (hFile == INVALID_HANDLE_VALUE) return;
        }
        LARGE_INTEGER pos; pos.QuadPart = lastSize.QuadPart;
        SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN);
        LONGLONG toRead = sz.QuadPart - lastSize.QuadPart;
        if (toRead > 65536) { pos.QuadPart = sz.QuadPart - 65536; SetFilePointerEx(hFile, pos, nullptr, FILE_BEGIN); toRead = 65536; }
        std::string buf((size_t)toRead, '\0');
        DWORD read = 0;
        if (!ReadFile(hFile, &buf[0], (DWORD)toRead, &read, nullptr)) return;
        buf.resize(read);
        lastSize = sz;

        size_t start = 0;
        for (size_t i = 0; i < buf.size(); ++i) {
            if (buf[i] == '\n') {
                std::string line = buf.substr(start, i - start);
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty()) {
                    lines.push_back(std::move(line));
                    while (lines.size() > MAX_LINES) lines.pop_front();
                }
                start = i + 1;
            }
        }
        if (start < buf.size()) {
            std::string line = buf.substr(start);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty()) {
                lines.push_back(std::move(line));
                while (lines.size() > MAX_LINES) lines.pop_front();
            }
        }
    }
};

// --------------------------------------------------------------- console -----
void set_color(WORD attr) { SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), attr); }
void reset_color() { set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE); }

void clear_screen() {
    HANDLE hc = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    GetConsoleScreenBufferInfo(hc, &csbi);
    DWORD cells = csbi.dwSize.X * csbi.dwSize.Y;
    COORD origin = {0, 0};
    DWORD written;
    FillConsoleOutputCharacterW(hc, L' ', cells, origin, &written);
    FillConsoleOutputAttribute(hc, csbi.wAttributes, cells, origin, &written);
    SetConsoleCursorPosition(hc, origin);
}

enum class State { NoProcess, NotInjected, Injected, Error };

void print_status(State st, DWORD pid, const std::wstring& err, const LogTail& log, int log_lines_to_show) {
    clear_screen();

    // Header
    set_color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
    std::wprintf(L"  DSX BT Throttle v" DSXT_VERSION_W L"\n");
    reset_color();
    std::wprintf(L"  ========================\n\n");

    // Status
    std::wprintf(L"  Status: ");
    switch (st) {
        case State::NoProcess:
            set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::wprintf(L"DSX not running\n");
            break;
        case State::NotInjected:
            set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::wprintf(L"DSX running (pid %lu) - NOT injected\n", pid);
            break;
        case State::Injected:
            set_color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            std::wprintf(L"ACTIVE - injected into DSX (pid %lu)\n", pid);
            break;
        case State::Error:
            set_color(FOREGROUND_RED | FOREGROUND_INTENSITY);
            std::wprintf(L"ERROR: %s\n", err.c_str());
            break;
    }
    reset_color();
    std::wprintf(L"\n");

    // Commands
    std::wprintf(L"  Commands:\n");
    std::wprintf(L"    [I] Inject    [R] Re-inject (restart DSX first)    [Q] Quit\n");
    std::wprintf(L"    [L] Show more log    [C] Clear log    [S] Refresh status\n");
    std::wprintf(L"\n");

    // Log
    std::wprintf(L"  --- Log (last %d lines) ---\n", log_lines_to_show);
    if (log.lines.empty()) {
        set_color(FOREGROUND_RED | FOREGROUND_GREEN);
        std::wprintf(L"  (no log yet)\n");
        reset_color();
    } else {
        int start = (int)log.lines.size() - log_lines_to_show;
        if (start < 0) start = 0;
        for (int i = start; i < (int)log.lines.size(); ++i) {
            const std::string& line = log.lines[i];
            if (line.find("[WRN]") != std::string::npos) set_color(FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            else if (line.find("DualSense output detected") != std::string::npos) set_color(FOREGROUND_GREEN | FOREGROUND_INTENSITY);
            else if (line.find("stats h=") != std::string::npos) set_color(FOREGROUND_BLUE | FOREGROUND_GREEN);
            else if (line.find("waiting for") != std::string::npos) set_color(FOREGROUND_RED | FOREGROUND_GREEN);
            else reset_color();
            std::printf("  %s\n", line.c_str());
        }
        reset_color();
    }

    std::wprintf(L"\n  > ");
    std::fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc > 1) {
        std::wprintf(
            L"dsx_throttle_injector v" DSXT_VERSION_W L"\n"
            L"Interactive console: finds DSX.exe, injects dsx_throttle.dll from the\n"
            L"same directory, auto-reinjects when DSX restarts and shows the live log.\n"
            L"Takes no arguments. Run as administrator (DSX is elevated).\n");
        return argv[1] == std::wstring(L"--help") ? 0 : 2;
    }

    std::wstring dll = exe_dir() + L"\\dsx_throttle.dll";
    wchar_t full[MAX_PATH * 2];
    if (!GetFullPathNameW(dll.c_str(), (DWORD)(sizeof full / sizeof full[0]), full, nullptr) || GetFileAttributesW(full) == INVALID_FILE_ATTRIBUTES) {
        std::wprintf(L"DLL not found: %s\n", dll.c_str());
        std::wprintf(L"Press Enter to exit...\n");
        std::getwchar();
        return 1;
    }
    dll = full;
    std::wstring dll_name = dll.substr(dll.find_last_of(L"\\/") + 1);
    std::wstring log_path = exe_dir() + L"\\dsx_throttle.log";

    SetConsoleTitleW(L"DSX BT Throttle");

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    DWORD oldMode = 0;
    GetConsoleMode(hIn, &oldMode);

    LogTail log;
    log.set_path(log_path);
    log.poll();

    State state = State::NoProcess;
    DWORD current_pid = 0;
    std::wstring last_error;
    int log_lines = 20;
    bool need_redraw = true;

    // DSX spawns helper processes; the one that talks to the controller has HidSharp loaded.
    auto scan = [&]() {
        current_pid = 0;
        state = State::NoProcess;
        for (const Target& t : find_dsx(dll_name)) {
            if (!t.has_hid) continue;
            current_pid = t.pid;
            state = t.has_dll ? State::Injected : State::NotInjected;
            break;
        }
    };

    auto do_inject = [&]() {
        if (current_pid == 0) {
            scan();
            if (current_pid == 0) {
                state = State::Error;
                last_error = L"DSX main process not found. Start DSX first.";
                need_redraw = true;
                return;
            }
        }
        std::wstring err;
        if (inject(current_pid, dll, err)) {
            state = State::Injected;
            log.close();
            log.lines.clear();
        } else {
            state = State::Error;
            last_error = err;
        }
        need_redraw = true;
    };

    scan();
    DWORD last_scan_tick = GetTickCount();

    for (;;) {
        if (need_redraw) {
            log.poll();
            print_status(state, current_pid, last_error, log, log_lines);
            need_redraw = false;
        }

        // Wait for input with timeout (poll log and re-scan periodically)
        SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT | ENABLE_WINDOW_INPUT);
        DWORD wait = WaitForSingleObject(hIn, 2000);

        if (wait == WAIT_OBJECT_0) {
            INPUT_RECORD ir[8];
            DWORD count = 0;
            ReadConsoleInputW(hIn, ir, 8, &count);
            for (DWORD i = 0; i < count; ++i) {
                if (ir[i].EventType != KEY_EVENT || !ir[i].Event.KeyEvent.bKeyDown) continue;
                wchar_t ch = ir[i].Event.KeyEvent.uChar.UnicodeChar;
                if (ch == 0) continue;
                ch = (wchar_t)towupper(ch);

                switch (ch) {
                    case L'I':
                        do_inject();
                        break;
                    case L'R':
                        scan();
                        if (state != State::Injected) do_inject();
                        need_redraw = true;
                        break;
                    case L'Q':
                        SetConsoleMode(hIn, oldMode);
                        reset_color();
                        return 0;
                    case L'L':
                        log_lines = (std::min)(log_lines + 20, 100);
                        need_redraw = true;
                        break;
                    case L'C':
                        log.lines.clear();
                        log_lines = 20;
                        need_redraw = true;
                        break;
                    case L'S':
                        scan();
                        need_redraw = true;
                        break;
                    default:
                        break;
                }
            }
        }

        size_t old_line_count = log.lines.size();
        log.poll();
        if (log.lines.size() != old_line_count) need_redraw = true;

        DWORD now = GetTickCount();
        if (now - last_scan_tick > 5000) {
            State old = state;
            DWORD old_pid = current_pid;
            scan();
            last_scan_tick = now;
            if (state != old || current_pid != old_pid) need_redraw = true;
        }

        if (state == State::NotInjected) do_inject();
    }
}
