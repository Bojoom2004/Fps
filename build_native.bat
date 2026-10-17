@echo off
rem Builds fps_booster.exe and afx_hook.dll (MSVC, C++17, C++/WinRT).
rem Run from "x64 Native Tools Command Prompt for VS 2019/2022".
rem Needs: Visual Studio Build Tools with "Desktop development with C++" and a Windows 10/11 SDK (10.0.17763 or newer).
setlocal
where cl >nul 2>nul
if errorlevel 1 (
  echo [!] cl.exe not found. Open "x64 Native Tools Command Prompt for VS" and run this script again.
  exit /b 1
)
cd /d "%~dp0"

echo === fps_booster.exe ===
cl /nologo /std:c++17 /EHsc /O2 /MT /permissive- /DUNICODE /D_UNICODE fps_booster.cpp /Fe:fps_booster.exe /link /SUBSYSTEM:CONSOLE
if errorlevel 1 goto fail

echo === afx_hook.dll ===
cl /nologo /std:c++17 /EHsc /O2 /MT /LD afx_hook.cpp /Fe:afx_hook.dll
if errorlevel 1 goto fail

del /q *.obj *.exp *.lib 2>nul
echo Done: fps_booster.exe and afx_hook.dll are in %~dp0
exit /b 0

:fail
echo [!] Native build failed. See the compiler messages above.
exit /b 1
