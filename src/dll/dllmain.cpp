// Entry point of dsx_throttle.dll (the injectable build).
#include "hooks.h"

BOOL WINAPI DllMain(HINSTANCE inst, DWORD reason, LPVOID) {
    switch (reason) {
    case DLL_PROCESS_ATTACH:
        DisableThreadLibraryCalls(inst);
        dsxt::start(inst);
        break;
    case DLL_PROCESS_DETACH:
        dsxt::stop();
        break;
    }
    return TRUE;
}
