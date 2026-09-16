# AGENTS.md

## Project overview

DLL that loads into DSX.exe and throttles DualSense Bluetooth output reports so BT headphones get bandwidth. Two builds of the same objects: `version.dll` (drop-in, DSX imports it) and `dsx_throttle.dll` (for the injector).

## DSX → DualSense

- DSX sends output reports with `kernelbase!WriteFile`, buffer **547 bytes**, `buf[0] == 0x31`. Not 78. The HID stack requires the buffer to match the descriptor's largest output report; bytes 78..546 are zero. ~320 calls/s idle.
- The device handle is opened before any DLL can be injected, so `CreateFileW` path matching never sees it. Detection is by buffer content only.
- Dead ends already measured at 0 calls: `HidD_SetOutputReport`, `DeviceIoControl` with HID codes, `ws2_32!send`/`WSASend`. `HidSharp.dll` only enumerates (open/close every second, never writes). `Nefarius.Utilities.Bluetooth` is radio management, no data path. `hidapi.dll` is not loaded.
- Hook `kernelbase` only. `kernel32!WriteFile` forwards to it; hooking both fires every hook twice and crashed DSX.
- Hooks must be removed in `DllMain(DLL_PROCESS_DETACH)` (`dsxt::stop()`). Our DLL is detached first at exit; a still-installed `CloseHandle` hook then hits destroyed globals → `0xC0000005` on DSX exit. `proxy_smoke.exe` covers this.
- The logger writes via `fprintf` → `WriteFile`; `HookGuard` (thread-local depth) keeps the hook from re-entering. `Log` uses a recursive mutex for the same reason.
- `dup_delay_ms` must default to `0`. Blocking the caller on dropped duplicates throttled DSX's whole output loop to ~60 Hz.
- CRC: CRC32 over `0xA2` + first 74 bytes, little-endian at 74..77. Seq is the high nibble of byte 1. Dedup key is bytes 2..73. Verified against live DSX packets (`crc_fail=0`).
- Only DualSense (and Edge, same protocol) is handled. DualShock 4 BT uses report `0x11` and passes through.

## Build and test

- `build.bat` builds everything and runs `tests.exe` + `proxy_smoke.exe`. Non-zero exit means a test failed, not just a compile error.
- **Close DSX before rebuilding.** The DLL is loaded in it; the linker fails with `LNK1168`. Check with `Get-Process DSX`.
- Live test: copy `bin\version.dll` + `bin\dsx_throttle.ini` to `C:\Program Files (x86)\Steam\steamapps\common\DSX\Main_v3_Beta\`, start DSX, read `dsx_throttle.log` there. Expected: `hooks enabled` → `DualSense output detected` → `stats` with `out` ≪ `in`.
- When something is unexplained, add a *counting* diagnostic (group by handle/size/ioctl, dump once after 20 s) and get the answer in one run. Do not iterate on guesses; each cycle costs the user a DSX restart.
- The user tests on the real setup and reports by screenshot. Ask for the log file, not the screen.

## Code style

- `src/dll/throttle.*` has no Win32 includes; everything testable lives there.
- Version string only in `src/dll/version.h`.
- A new hook needs a cheap content pre-filter before touching any global, and must respect `g_active` and `HookGuard`.
- `/W4` clean. No comments that restate the code.
- Docs and code in English; talk to the user in Russian.
