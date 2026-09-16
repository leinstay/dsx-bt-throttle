// Smoke test for the version.dll drop-in: loads it from a scratch directory,
// checks that the forwarded exports really reach System32\version.dll and
// that the throttle initialises (log says "hooks enabled").
// Usage: proxy_smoke.exe <path-to-version.dll>
#define WIN32_LEAN_AND_MEAN
#define _CRT_SECURE_NO_WARNINGS
#include <windows.h>

#include <cstdio>
#include <string>

static int g_fail = 0, g_pass = 0;
#define CHECK(cond) do { if (cond) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } } while (0)

static std::string read_all(const std::wstring& p) {
    FILE* f = _wfopen(p.c_str(), L"rb"); if (!f) return {};
    std::string s; char buf[4096]; size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
    fclose(f); return s;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) { std::printf("usage: proxy_smoke <version.dll>\n"); return 2; }

    wchar_t tmp[MAX_PATH]; GetTempPathW(MAX_PATH, tmp);
    std::wstring work = std::wstring(tmp) + L"dsx_throttle_smoke";
    CreateDirectoryW(work.c_str(), nullptr);
    std::wstring dll = work + L"\\version.dll", ini = work + L"\\dsx_throttle.ini", logp = work + L"\\dsx_throttle.log";
    DeleteFileW(logp.c_str());
    if (!CopyFileW(argv[1], dll.c_str(), FALSE)) { std::printf("copy failed: %lu\n", GetLastError()); return 1; }
    { FILE* f = _wfopen(ini.c_str(), L"w"); std::fprintf(f, "[throttle]\nstats_interval_s=0\n"); std::fclose(f); }

    HMODULE m = LoadLibraryW(dll.c_str());
    CHECK(m != nullptr);
    if (!m) { std::printf("LoadLibrary failed: %lu\n", GetLastError()); return 1; }

    HMODULE sys = GetModuleHandleW(L"C:\\Windows\\System32\\version.dll");
    if (!sys) { wchar_t s[MAX_PATH]; GetSystemDirectoryW(s, MAX_PATH); sys = GetModuleHandleW((std::wstring(s) + L"\\version.dll").c_str()); }
    CHECK(sys != nullptr && sys != m);   // the real DLL is loaded alongside ours

    typedef DWORD (WINAPI *SizeW_fn)(LPCWSTR, LPDWORD);
    typedef BOOL  (WINAPI *InfoW_fn)(LPCWSTR, DWORD, DWORD, LPVOID);
    typedef BOOL  (WINAPI *QueryW_fn)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    typedef DWORD (WINAPI *LangA_fn)(DWORD, LPSTR, DWORD);

    auto SizeW  = (SizeW_fn) GetProcAddress(m, "GetFileVersionInfoSizeW");
    auto InfoW  = (InfoW_fn) GetProcAddress(m, "GetFileVersionInfoW");
    auto QueryW = (QueryW_fn)GetProcAddress(m, "VerQueryValueW");
    auto LangA  = (LangA_fn) GetProcAddress(m, "VerLanguageNameA");
    CHECK(SizeW && InfoW && QueryW && LangA);
    for (const char* name : { "GetFileVersionInfoA", "GetFileVersionInfoByHandle", "GetFileVersionInfoExA", "GetFileVersionInfoExW",
                              "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA", "GetFileVersionInfoSizeExW", "VerFindFileA",
                              "VerFindFileW", "VerInstallFileA", "VerInstallFileW", "VerLanguageNameW", "VerQueryValueA" })
        CHECK(GetProcAddress(m, name) != nullptr);

    // Round trip through three forwarded exports on a file that certainly has a version resource.
    wchar_t k32[MAX_PATH]; GetSystemDirectoryW(k32, MAX_PATH); wcscat_s(k32, L"\\kernel32.dll");
    DWORD handle = 0, size = SizeW ? SizeW(k32, &handle) : 0;
    CHECK(size > 0);
    if (size) {
        std::string blob(size, '\0');
        CHECK(InfoW(k32, 0, size, &blob[0]));
        VS_FIXEDFILEINFO* ffi = nullptr; UINT len = 0;
        CHECK(QueryW(blob.data(), L"\\", (LPVOID*)&ffi, &len) && ffi && ffi->dwSignature == 0xFEEF04BD);
    }
    char lang[64] = {};
    CHECK(LangA(0x0409, lang, sizeof lang) > 0 && lang[0] != '\0');   // "English (United States)"

    // The throttle started on its own thread; give it a moment.
    std::string log;
    for (int i = 0; i < 50 && log.find("hooks enabled") == std::string::npos; ++i) { Sleep(100); log = read_all(logp); }
    CHECK(log.find("dsx_throttle ") != std::string::npos);
    CHECK(log.find("hooked kernelbase.dll!WriteFile") != std::string::npos);
    CHECK(log.find("hooks enabled") != std::string::npos);
    CHECK(log.find("[WRN]") == std::string::npos);

    // Unload while hooks are live: DllMain(DETACH) must restore WriteFile/CloseHandle,
    // otherwise the printf below jumps into unmapped memory. Same path runs at process exit.
    CHECK(FreeLibrary(m));
    CHECK(GetModuleHandleW(dll.c_str()) == nullptr);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    CHECK(ev && CloseHandle(ev));
    std::printf("post-unload WriteFile ok\n");
    log = read_all(logp);
    CHECK(log.find("unloading") != std::string::npos);

    std::printf("%d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
