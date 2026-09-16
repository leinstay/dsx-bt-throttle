# Contributing

## Setup

1. `git clone --recursive <repo-url>`
2. Visual Studio 2022+ with the C++ desktop workload
3. `build.bat` builds everything and runs the tests

## Changes

1. Branch from `main`
2. `build.bat` must pass — it fails on any test failure
3. For changes to `hooks.cpp` or the proxy, run against DSX with a Bluetooth controller and check `dsx_throttle.log`
4. Open a PR

## Code Style

- C++17, MSVC, `/W4` clean, static CRT (`/MT`)
- `src/dll/throttle.*` stays free of Win32 headers; it is the unit-tested part
- Code inside a hook does the minimum: DSX calls `WriteFile` from several threads
- Comments explain why, not what
- The version string lives only in `src/dll/version.h`

## Tests

`build.bat` runs:

- `build\tests.exe` — unit tests for the core
- `build\proxy_smoke.exe bin\version.dll` — loads the drop-in from a scratch folder, calls forwarded `version.dll` exports, checks the log says `hooks enabled`, unloads it and verifies `WriteFile`/`CloseHandle` still work

Manual check: `stats` lines with `out` far below `in`; changing a lightbar colour in DSX reaches the controller and `out` ticks up. `trace=1` shows every decision.

## Issues

Attach `dsx_throttle.log` (ideally with `trace=1` for the first minute) and your `dsx_throttle.ini` if you changed it.
