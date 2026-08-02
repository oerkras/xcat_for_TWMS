@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

for /f %%a in ('echo prompt $E ^| cmd') do set "ESC=%%a"
set "C_GREEN=!ESC![92m"
set "C_RED=!ESC![91m"
set "C_YELLOW=!ESC![93m"
set "C_RESET=!ESC![0m"

set "BUILD_DIR=build"
set "CONFIG=Release"
if not "%~1"=="" set "CONFIG=%~1"
if /I not "%CONFIG%"=="Release" (
    echo !C_RED!FAILED: package-release only supports Release builds. Got "%CONFIG%".!C_RESET!
    exit /b 1
)

set "PRODUCT_NAME=xcat_for_twms"
set "ARTIFACT_RELEASE_DIR=artifacts\release"
set "VERSION_FILE=common\xcat_version_rc.h"
set "VERSION_BACKUP=%ARTIFACT_RELEASE_DIR%\.xcat_version.package-release.backup.h"
set "PUBLISH_SITE_SCRIPT=publish_site\publish-latest-from-repo.ps1"

if exist "%VERSION_BACKUP%" (
    echo !C_YELLOW!WARN: found a previous package-release version backup; restoring %VERSION_FILE%.!C_RESET!
    copy /Y "%VERSION_BACKUP%" "%VERSION_FILE%" >nul
    if errorlevel 1 (
        echo !C_RED!FAILED: cannot restore stale %VERSION_FILE% backup.!C_RESET!
        exit /b 1
    )
    del "%VERSION_BACKUP%" >nul 2>&1
)

where node >nul 2>&1
if errorlevel 1 (
    echo !C_RED!FAILED: node not found in PATH.!C_RESET!
    exit /b 1
)

where cmake >nul 2>&1
if errorlevel 1 (
    echo !C_RED!FAILED: cmake not found in PATH.!C_RESET!
    exit /b 1
)

tasklist /FI "IMAGENAME eq xcat.exe" 2>nul | find /I "xcat.exe" >nul
if not errorlevel 1 (
    echo !C_RED!FAILED: xcat.exe is running. Close it before packaging.!C_RESET!
    exit /b 1
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"
if not exist "%ARTIFACT_RELEASE_DIR%" mkdir "%ARTIFACT_RELEASE_DIR%"

echo [package-release] CMake configure...
cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo !C_RED!FAILED: CMake configure error.!C_RESET!
    exit /b 1
)

copy /Y "%VERSION_FILE%" "%VERSION_BACKUP%" >nul
if errorlevel 1 (
    echo !C_RED!FAILED: cannot backup %VERSION_FILE%.!C_RESET!
    exit /b 1
)

echo [package-release] Bumping version...
node "%~dp0scripts\bump-version.mjs"
if errorlevel 1 (
    echo !C_RED!FAILED: version bump error.!C_RESET!
    copy /Y "%VERSION_BACKUP%" "%VERSION_FILE%" >nul
    del "%VERSION_BACKUP%" >nul 2>&1
    exit /b 1
)

echo [package-release] Building %PRODUCT_NAME% (%CONFIG%)...
cmake --build "%BUILD_DIR%" --config "%CONFIG%" --target xcat xcat_probe
if errorlevel 1 (
    echo !C_RED!FAILED: compile/link error.!C_RESET!
    echo !C_YELLOW!HINT: If LNK1104, close bin\xcat.exe ^(and anything locking bin\XCat_data\xcat.dll^) then retry.!C_RESET!
    copy /Y "%VERSION_BACKUP%" "%VERSION_FILE%" >nul
    del "%VERSION_BACKUP%" >nul 2>&1
    exit /b 1
)

echo [package-release] Packing zip...
node "%~dp0scripts\package-release.mjs"
if errorlevel 1 (
    echo !C_RED!FAILED: package script error.!C_RESET!
    copy /Y "%VERSION_BACKUP%" "%VERSION_FILE%" >nul
    del "%VERSION_BACKUP%" >nul 2>&1
    exit /b 1
)

echo [package-release] Publishing to publish_site...
powershell -NoProfile -ExecutionPolicy Bypass -File "%PUBLISH_SITE_SCRIPT%"
if errorlevel 1 (
    echo !C_RED!FAILED: publish_site sync failed; zip is in artifacts\release.!C_RESET!
    echo !C_YELLOW!HINT: version backup kept. Prefer retry publish only ^(avoids rebuild^):!C_RESET!
    echo !C_YELLOW!      powershell -NoProfile -ExecutionPolicy Bypass -File "%PUBLISH_SITE_SCRIPT%"!C_RESET!
    echo !C_YELLOW!      Re-running this bat is version-safe: it restores the pre-bump header then bumps once.!C_RESET!
    exit /b 1
)

del "%VERSION_BACKUP%" >nul 2>&1

echo.
echo !C_GREEN!OK: %PRODUCT_NAME% packaged and published.!C_RESET!
echo        Update API:   scripts\start-twms-update-server.ps1
echo        Manifest:     http://xcat.work:18789/twms/update/latest.json
echo        Web downloads: http://xcat.work:52080/  ^(publish_site\start-server.ps1^)
exit /b 0
