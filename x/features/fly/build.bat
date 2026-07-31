@echo off
setlocal
set "VCVARS=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
if not exist "%VCVARS%" (
  echo vcvars64.bat not found
  exit /b 1
)
call "%VCVARS%"
cd /d "%~dp0"
if not exist "..\..\..\Dumps\runtime\out_bin" mkdir "..\..\..\Dumps\runtime\out_bin"
rem TWMS_FLY_STANDALONE enables DllMain so this DLL can still be injected alone.
cl /nologo /O2 /LD /EHsc /W3 /DUNICODE /D_UNICODE /DTWMS_FLY_STANDALONE /I. /I..\kick_sniff ^
  fly.cpp fly_impl.cpp ..\kick_sniff\kick_sniff.cpp ^
  /Fe:..\..\..\Dumps\runtime\out_bin\TwmsFly.dll ^
  /link /DLL /MACHINE:X64 winmm.lib Psapi.lib User32.lib
if errorlevel 1 exit /b 1
echo.
echo Built: %~dp0..\..\..\Dumps\runtime\out_bin\TwmsFly.dll
dir "%~dp0..\..\..\Dumps\runtime\out_bin\TwmsFly.dll"
