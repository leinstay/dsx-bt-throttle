@echo off
setlocal EnableDelayedExpansion
rem Builds everything into bin\ (x64, static CRT):
rem   version.dll               drop-in for the DSX folder (auto-loads)
rem   dsx_throttle.dll          same code, for the injector
rem   dsx_throttle_injector.exe console injector + log viewer
rem   build\tests.exe           unit tests (run automatically)

set ROOT=%~dp0
set ROOT=%ROOT:~0,-1%
set OBJ=%ROOT%\build\obj
set BIN=%ROOT%\bin
set MH=%ROOT%\third_party\minhook

if not exist "%MH%\src\hook.c" (
  echo MinHook sources not found. Run:
  echo   git submodule update --init
  exit /b 1
)

if defined VCToolsInstallDir goto :env_ok
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" ( echo vswhere.exe not found - install Visual Studio with C++ tools & exit /b 1 )
"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%TEMP%\dsxt_vsdir.txt"
set /p VSDIR=<"%TEMP%\dsxt_vsdir.txt"
del "%TEMP%\dsxt_vsdir.txt" >nul 2>&1
if not defined VSDIR ( echo Visual Studio with C++ tools not found & exit /b 1 )
echo Using %VSDIR%
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:env_ok

if not exist "%OBJ%" mkdir "%OBJ%"
if not exist "%BIN%" mkdir "%BIN%"

set CFLAGS=/nologo /O2 /MT /W4 /EHsc /std:c++17 /DUNICODE /D_UNICODE /DNOMINMAX /Zi /Fd"%OBJ%\\"
set CCFLAGS=/nologo /O2 /MT /W3 /DWIN32_LEAN_AND_MEAN /I"%MH%\include"
set CORE_OBJS="%OBJ%\throttle.obj" "%OBJ%\log.obj" "%OBJ%\config.obj" "%OBJ%\hooks.obj"
set MH_OBJS="%OBJ%\buffer.obj" "%OBJ%\hook.obj" "%OBJ%\trampoline.obj" "%OBJ%\hde64.obj"
set DLL_LIBS=kernel32.lib winmm.lib
set LINKFLAGS=/nologo /DEBUG /INCREMENTAL:NO /OPT:REF /OPT:ICF

echo [1/5] MinHook
cl %CCFLAGS% /c /Fo"%OBJ%\\" "%MH%\src\buffer.c" "%MH%\src\hook.c" "%MH%\src\trampoline.c" "%MH%\src\hde\hde64.c" || exit /b 1

echo [2/5] throttle core
cl %CFLAGS% /I"%MH%\include" /c /Fo"%OBJ%\\" "%ROOT%\src\dll\throttle.cpp" "%ROOT%\src\dll\log.cpp" "%ROOT%\src\dll\config.cpp" "%ROOT%\src\dll\hooks.cpp" "%ROOT%\src\dll\dllmain.cpp" || exit /b 1

echo [3/5] dsx_throttle.dll + version.dll
link %LINKFLAGS% /DLL /OUT:"%BIN%\dsx_throttle.dll" /PDB:"%BIN%\dsx_throttle.pdb" ^
  "%OBJ%\dllmain.obj" %CORE_OBJS% %MH_OBJS% %DLL_LIBS% || exit /b 1
cl %CFLAGS% /c /Fo"%OBJ%\\" "%ROOT%\src\proxy\proxy.cpp" || exit /b 1
ml64 /nologo /c /Fo"%OBJ%\trampolines.obj" "%ROOT%\src\proxy\trampolines.asm" || exit /b 1
link %LINKFLAGS% /DLL /OUT:"%BIN%\version.dll" /PDB:"%BIN%\version.pdb" /DEF:"%ROOT%\src\proxy\proxy.def" ^
  "%OBJ%\proxy.obj" "%OBJ%\trampolines.obj" %CORE_OBJS% %MH_OBJS% %DLL_LIBS% || exit /b 1

echo [4/5] dsx_throttle_injector.exe
cl %CFLAGS% /c /Fo"%OBJ%\\" "%ROOT%\src\injector\injector.cpp" || exit /b 1
link %LINKFLAGS% /OUT:"%BIN%\dsx_throttle_injector.exe" /PDB:"%BIN%\dsx_throttle_injector.pdb" /SUBSYSTEM:CONSOLE ^
  /MANIFEST:EMBED /MANIFESTUAC:"level='requireAdministrator' uiAccess='false'" ^
  "%OBJ%\injector.obj" kernel32.lib psapi.lib || exit /b 1

echo [5/5] tests
cl %CFLAGS% /wd4389 /c /Fo"%OBJ%\\" "%ROOT%\src\tests\test_main.cpp" "%ROOT%\src\tests\proxy_smoke.cpp" || exit /b 1
link /nologo /OUT:"%ROOT%\build\tests.exe" "%OBJ%\test_main.obj" "%OBJ%\throttle.obj" || exit /b 1
link /nologo /OUT:"%ROOT%\build\proxy_smoke.exe" "%OBJ%\proxy_smoke.obj" kernel32.lib || exit /b 1
"%ROOT%\build\tests.exe" || exit /b 1
"%ROOT%\build\proxy_smoke.exe" "%BIN%\version.dll" || exit /b 1

if not exist "%BIN%\dsx_throttle.ini" copy /y "%ROOT%\dsx_throttle.ini" "%BIN%\dsx_throttle.ini" >nul
del /q "%BIN%\*.exp" "%BIN%\*.lib" "%BIN%\*.ilk" >nul 2>&1

echo.
echo Build OK: %BIN%
dir /b "%BIN%"
