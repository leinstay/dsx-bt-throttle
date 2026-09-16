#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace dsxt {

// Loads the ini next to `self`, opens the log and installs the hooks on a
// background thread. Safe to call from DllMain(DLL_PROCESS_ATTACH).
void start(HMODULE self);

// Removes the hooks and closes the log. Must run from
// DllMain(DLL_PROCESS_DETACH), i.e. before this module's static objects are
// destroyed, otherwise a CloseHandle from another DLL's teardown would hit a
// hook whose state is already gone.
void stop();

}  // namespace dsxt
