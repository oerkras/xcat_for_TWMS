@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem Repo-root helper: cross-sign bin\krw\xcat_krw.sys (expired leaf + WDK /ac).
rem Needs Administrator (clock backdate). Does NOT load the driver.
rem
rem Optional:
rem   sign_krw.bat [sys_path] [pfx_path]
rem Env override:
rem   XCAT_KRW_PFX / XCAT_KRW_PFX_PASSWORD / XCAT_KRW_CROSS_CER

cd /d "%~dp0"

net session >nul 2>&1
if errorlevel 1 (
  echo Elevating for BackdateForExpired...
  powershell -NoProfile -ExecutionPolicy Bypass -Command ^
    "Start-Process -FilePath '%~f0' -Verb RunAs -Wait"
  exit /b %ERRORLEVEL%
)

set "SYS=%~1"
if "%SYS%"=="" set "SYS=%~dp0bin\krw\xcat_krw.sys"
for %%I in ("%SYS%") do set "SYS=%%~fI"

if not exist "%SYS%" (
  echo [FAIL] sys not found: %SYS%
  echo Build tools\krw first: tools\krw\build_driver.bat
  exit /b 1
)

set "PFX=%~2"
if "%PFX%"=="" set "PFX=%XCAT_KRW_PFX%"
if "%PFX%"=="" (
  rem Prefer known-good VeriSign leaf used in prior KRW loads.
  for /f "delims=" %%F in ('dir /s /b "%~dp0DriverTools\GigaDevice*.pfx" 2^>nul') do (
    if not defined PFX set "PFX=%%F"
  )
)
if "%PFX%"=="" (
  for /f "delims=" %%F in ('dir /s /b "%~dp0DriverTools\*_Abc123456.pfx" 2^>nul') do (
    if not defined PFX set "PFX=%%F"
  )
)
if "%PFX%"=="" (
  echo [FAIL] No PFX. Put a VeriSign/Thawte/GeoTrust codesign .pfx under DriverTools\
  echo    or set XCAT_KRW_PFX / pass path as arg2.
  exit /b 2
)
for %%I in ("%PFX%") do set "PFX=%%~fI"

set "PASS=%XCAT_KRW_PFX_PASSWORD%"
if "%PASS%"=="" (
  rem Filename convention: Name_PASSWORD.pfx  e.g. GigaDevice_Abc123456.pfx
  for %%I in ("%PFX%") do set "PFXNAME=%%~nI"
  for /f "tokens=2 delims=_" %%P in ("!PFXNAME!") do set "PASS=%%P"
)
if "%PASS%"=="" set "PASS=Abc123456"

set "SIGN_PS=%~dp0tools\krw\scripts\sign_cross.ps1"
if not exist "%SIGN_PS%" (
  echo [FAIL] missing %SIGN_PS%
  exit /b 1
)

echo SYS=%SYS%
echo PFX=%PFX%
echo Running sign_cross.ps1 -BackdateForExpired -SkipTimestamp ...

powershell -NoProfile -ExecutionPolicy Bypass -File "%SIGN_PS%" ^
  -SysPath "%SYS%" -PfxPath "%PFX%" -PfxPassword "%PASS%" ^
  -BackdateForExpired -SkipTimestamp
set "RC=%ERRORLEVEL%"
if not "%RC%"=="0" (
  echo [FAIL] sign exit %RC%
  exit /b %RC%
)

echo [OK] signed: %SYS%
echo Copy to VM then load.bat / InstDrv. Do not leave driver loaded on host.
exit /b 0
