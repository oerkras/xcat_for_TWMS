@echo off
setlocal EnableExtensions
rem Lab load: fixed service name XCatKrw (NOT stealth UUID path).
rem Requires Administrator. Sys default: repo\bin\krw\xcat_krw.sys
rem Optional: load.bat "D:\path\to\xcat_krw.sys"

net session >nul 2>&1
if errorlevel 1 (
  echo [FAIL] Run as Administrator.
  exit /b 1
)

set "SERVICE=XCatKrw"
set "SYS=%~1"
if "%SYS%"=="" (
  set "SYS=%~dp0..\..\..\bin\krw\xcat_krw.sys"
)

for %%I in ("%SYS%") do set "SYS=%%~fI"
if not exist "%SYS%" (
  echo [FAIL] sys not found: %SYS%
  echo Build tools\krw first, then sign if needed.
  exit /b 1
)

echo NOTE: fixed name "%SERVICE%" = lab path. Stealth uses compat Init ^(SVC_uuid^).
echo binPath = %SYS%

sc.exe query "%SERVICE%" >nul 2>&1
if not errorlevel 1 (
  sc.exe query "%SERVICE%" | findstr /i "RUNNING" >nul
  if not errorlevel 1 (
    echo Service already RUNNING.
    exit /b 0
  )
  echo Starting existing service...
  sc.exe start "%SERVICE%"
  exit /b %ERRORLEVEL%
)

echo Creating service...
sc.exe create "%SERVICE%" type= kernel start= demand binPath= "%SYS%"
if errorlevel 1 exit /b %ERRORLEVEL%

echo Starting...
sc.exe start "%SERVICE%"
exit /b %ERRORLEVEL%
