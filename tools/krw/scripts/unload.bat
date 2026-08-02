@echo off
setlocal EnableExtensions
rem Lab unload: stop+delete fixed service XCatKrw.
rem Optional: unload.bat /image  — also stop/delete any service whose ImagePath contains xcat_krw.sys
rem   (covers stealth SVC_<uuid> loads from compat Init).
rem Requires Administrator.

net session >nul 2>&1
if errorlevel 1 (
  echo [FAIL] Run as Administrator.
  exit /b 1
)

set "SERVICE=XCatKrw"
set "DO_IMAGE=0"
if /i "%~1"=="/image" set "DO_IMAGE=1"
if /i "%~1"=="-ByImagePath" set "DO_IMAGE=1"

call :StopDelete "%SERVICE%"

if "%DO_IMAGE%"=="1" (
  echo Scanning services for ImagePath *xcat_krw.sys* ...
  for /f "skip=1 tokens=1*" %%A in ('reg query "HKLM\SYSTEM\CurrentControlSet\Services" 2^>nul') do (
    call :MaybeUnloadByImage "%%A"
  )
)

echo Done.
exit /b 0

:StopDelete
set "N=%~1"
sc.exe query "%N%" >nul 2>&1
if errorlevel 1 goto :eof
echo Stopping %N% ...
sc.exe stop "%N%" >nul 2>&1
timeout /t 1 /nobreak >nul
echo Deleting %N% ...
sc.exe delete "%N%"
goto :eof

:MaybeUnloadByImage
set "KEY=%~1"
if /i not "%KEY:~0,4%"=="HKEY" goto :eof
for /f "tokens=2*" %%V in ('reg query "%KEY%" /v ImagePath 2^>nul ^| findstr /i "ImagePath"') do set "IMG=%%W"
if not defined IMG goto :eof
echo %IMG% | findstr /i "xcat_krw.sys" >nul
if errorlevel 1 (
  set "IMG="
  goto :eof
)
for %%P in ("%KEY%") do set "SVCNAME=%%~nxP"
if /i "%SVCNAME%"=="%SERVICE%" (
  set "IMG="
  goto :eof
)
echo Matched ImagePath: %SVCNAME% ^(%IMG%^)
call :StopDelete "%SVCNAME%"
set "IMG="
goto :eof
