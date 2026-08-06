@echo off
setlocal EnableExtensions
REM ASCII-only launcher. All logic is in dump_system_logs.ps1
REM Right-click -> Run as administrator (recommended)

cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  echo [WARN] Not admin. Some logs may be skipped. Prefer Run as administrator.
) else (
  echo [OK] Admin rights acquired.
)

if not exist "%~dp0dump_system_logs.ps1" (
  echo [ERROR] dump_system_logs.ps1 not found next to this BAT.
  echo Copy BOTH files together:
  echo   dump_system_logs.bat
  echo   dump_system_logs.ps1
  pause
  exit /b 1
)

echo Starting dump...
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0dump_system_logs.ps1"
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" (
  echo [ERROR] PowerShell exited with code %RC%
)
pause
exit /b %RC%
