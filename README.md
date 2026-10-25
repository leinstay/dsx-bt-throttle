# DSX BT Throttle

A patch for DSX that fixes Bluetooth headphones failing to connect or dropping out while DSX is running with a Bluetooth DualSense. The problem is described in [this Steam discussion](https://steamcommunity.com/app/1812620/discussions/0/804595528394352826/).

DSX writes an output report to the controller several hundred times per second whether or not anything changed. This DLL loads inside `DSX.exe`, drops the reports that repeat the previous one and spaces the rest at least 16 ms apart. Adaptive triggers, lightbar and rumble keep working; the radio gets its bandwidth back.

## How it works

```
DSX.exe
   ↓  WriteFile(hDualSense, 547-byte HID buffer)   ~320 calls/s
dsx_throttle (inside DSX.exe)
   ↓  same content as last time?  → drop, report success to DSX
   ↓  sent < 16 ms ago?           → wait for the slot
   ↓  renumber seq, recompute CRC32
kernelbase!WriteFile → HID class driver → BTHPORT → controller   ≤ 62 packets/s
```

Only DualSense Bluetooth output reports (report id `0x31`) are touched. Everything else DSX writes passes through unchanged. Controller input is not modified.

## Requirements

Windows 10/11 x64, DSX v3 from Steam, DualSense connected over Bluetooth.

## Installation

### Drop-in

Copy `version.dll` and `dsx_throttle.ini` from `bin\` into the DSX folder, next to `DSX.exe`:

```bat
copy bin\version.dll      "C:\Program Files (x86)\Steam\steamapps\common\DSX\Main_v3_Beta\"
copy bin\dsx_throttle.ini "C:\Program Files (x86)\Steam\steamapps\common\DSX\Main_v3_Beta\"
```

`DSX.exe` imports `version.dll`; Windows picks up the copy next to the executable, which forwards every call to the real one in `System32` and starts the throttle. If a Steam update removes the file, copy it again.

### Injector

If you prefer not to put files in the DSX folder: keep `dsx_throttle_injector.exe`, `dsx_throttle.dll` and `dsx_throttle.ini` together and run the injector. It asks for elevation (DSX runs elevated), finds the DSX process that has `HidSharp.dll` loaded, injects the DLL with `CreateRemoteThread` and tails the log. It re-scans every 5 s and re-injects when DSX restarts. Some antivirus products flag the injection pattern; the drop-in does not use it.

## Checking that it works

`dsx_throttle.log` appears next to the DLL:

```
dsx_throttle 2.1.0 in pid 5916 (C:\...\Main_v3_Beta\DSX.exe)
hooked kernelbase.dll!WriteFile at 00007FF9696B2070
hooked kernelbase.dll!CloseHandle at 00007FF9696266A0
hooks enabled
DualSense output detected on h=00000000000012A8 (report id 0x31, seq/tag 0x02, flags 0x0F)
stats h=00000000000012A8: in=1231 (61/s) out=1 (0/s) dup=1229 delayed=0 err=0 crc_fail=0 | avg gap 0.0 ms | blocked 0.00 s
```

`in` is what DSX tried to send, `out` is what reached the controller. With an idle controller `out` stays near zero and rises when a profile, colour or trigger changes.

## Configuration

`dsx_throttle.ini` is read once when the DLL loads; restart DSX after editing it.

| Key | Default | Meaning |
|-----|---------|---------|
| `enabled` | `1` | `0` = pass everything through, only collect statistics |
| `dedup` | `1` | Drop reports whose payload equals the last one sent |
| `min_interval_ms` | `16` | Minimum gap between real sends. Lower if triggers feel laggy, raise if audio still stutters |
| `dup_delay_ms` | `0` | How long a dropped duplicate blocks the DSX writer thread. `-1` = same as `min_interval_ms` |
| `rewrite_seq_crc` | `1` | Renumber the sequence nibble and recompute CRC32 on forwarded packets |
| `trace` | `0` | One log line per write; stops after `trace_max_lines` |
| `stats_interval_s` | `10` | Period of the `stats` lines, `0` = off |
| `log`, `log_path`, `log_append` | `1`, `dsx_throttle.log`, `0` | Log file settings; the path is relative to the DLL directory |

## Building

Visual Studio 2022 or newer with the C++ desktop workload.

```bat
git clone --recursive https://github.com/leinstay/dsx-bt-throttle.git
cd dsx-bt-throttle
build.bat
```

`build.bat` builds both DLLs, the injector and the tests, runs the tests and leaves the binaries in `bin\`. Close DSX before rebuilding, otherwise the linker cannot overwrite the loaded DLL.

## Layout

```
src/dll/throttle.*     CRC32, dedup, rate limit, no Win32, unit-tested
src/dll/hooks.*        MinHook hooks on kernelbase!WriteFile / CloseHandle
src/dll/config.*       dsx_throttle.ini
src/dll/log.*          file logger
src/dll/dllmain.cpp    dsx_throttle.dll entry point
src/proxy/             version.dll entry point, export table, asm trampolines
src/injector/          console injector
src/tests/             unit tests and proxy smoke test
third_party/minhook/   git submodule
```

## License and credits

MIT License, see [LICENSE](LICENSE).

* [MinHook](https://github.com/TsudaKageyu/minhook) by Tsuda Kageyu: BSD-2-Clause License
* DualSense report layout from [controllers.fandom.com](https://controllers.fandom.com/wiki/Sony_DualSense/Data_Structures)
