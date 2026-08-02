@echo off
setlocal
rem Build tools/krw with VS + WDK. Output: bin\krw\
cd /d "%~dp0"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo vswhere not found. Install Visual Studio Build Tools.
  exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSINSTALL=%%i"
if not defined VSINSTALL (
  echo Visual Studio with C++ tools not found.
  exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" || exit /b 1

msbuild "%~dp0xcat_krw.sln" /m /p:Configuration=Release /p:Platform=x64 /p:SpectreMitigation=false
if errorlevel 1 exit /b %ERRORLEVEL%

rem Optional cross-sign when XCAT_KRW_PFX is set (22H2 VM / Secure Boot OFF path).
if defined XCAT_KRW_PFX (
  echo Cross-signing with XCAT_KRW_PFX...
  powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\sign_cross.ps1"
  exit /b %ERRORLEVEL%
)
exit /b 0
