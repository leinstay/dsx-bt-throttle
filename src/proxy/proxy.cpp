// Entry point of version.dll (the drop-in build).
//
// DSX.exe imports version.dll; a copy placed next to the exe wins over the
// one in System32 (DLL search order), so this file gets loaded at startup with
// no injector involved. Every export is forwarded to the real System32 copy
// through the jump table in trampolines.asm; proxy.def maps the names.
#include "../dll/hooks.h"

extern "C" FARPROC fp[17] = {};

static const char* const kExports[17] = {
    "GetFileVersionInfoA", "GetFileVersionInfoByHandle",
    "GetFileVersionInfoExA", "GetFileVersionInfoExW",
    "GetFileVersionInfoSizeA", "GetFileVersionInfoSizeExA",
    "GetFileVersionInfoSizeExW", "GetFileVersionInfoSizeW",
    "GetFileVersionInfoW",
    "VerFindFileA", "VerFindFileW",
    "VerInstallFileA", "VerInstallFileW",
    "VerLanguageNameA", "VerLanguageNameW",
    "VerQueryValueA", "VerQueryValueW",
};

static bool load_system_version_dll() {
    wchar_t path[MAX_PATH];
    UINT n = GetSystemDirectoryW(path, MAX_PATH);
    if (!n || n >= MAX_PATH - 12) return false;
    wcscat_s(path, L"\\version.dll");
    HMODULE real = LoadLibraryW(path);
    if (!real) return false;
    for (int i = 0; i < 17; ++i) fp[i] = GetProcAddress(real, kExports[i]);
    return true;
}

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(inst);
        if (!load_system_version_dll()) return FALSE;  // refuse to load rather than break DSX
        dsxt::start(inst);
        break;
    case DLL_PROCESS_DETACH:
        dsxt::stop();
        break;
    }
    return TRUE;
}
