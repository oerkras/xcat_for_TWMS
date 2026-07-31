@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

for /f %%a in ('echo prompt $E ^| cmd') do set "ESC=%%a"
set "C_GREEN=!ESC![92m"
set "C_RED=!ESC![91m"
set "C_YELLOW=!ESC![93m"
set "C_RESET=!ESC![0m"

if not defined VSCMD_VER (
    pushd "%~dp0"
    call "%~dp0scripts\dev-env.bat"
    set "_DEVENV_ERR=!errorlevel!"
    popd
    if !_DEVENV_ERR! neq 0 (
        echo !C_RED!ERROR: Visual Studio 2022 environment not available.!C_RESET!
        exit /b 1
    )
)

set PRODUCT_NAME=xcat_for_TWMS
set BUILD_DIR=build_xcat_for_TWMS
set CONFIG=Release
if not "%~1"=="" set CONFIG=%~1

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo.
    echo !C_RED!Build FAILED: CMake configure error.!C_RESET!
    exit /b 1
)

rem Main target: bin\xcat.exe (silent WebView ticket inside).
rem On LNK1104: close bin\xcat.exe yourself, then re-run. This script never kills processes.
cmake --build "%BUILD_DIR%" --config %CONFIG% --target xcat_sound
if errorlevel 1 (
    echo !C_YELLOW!Build WARNING: xcat_sound.exe was not built; continuing without build sound.!C_RESET!
)

cmake --build "%BUILD_DIR%" --config %CONFIG% --target xcat xcat_probe
if errorlevel 1 (
    echo.
    echo !C_RED!Build FAILED: compile/link error.!C_RESET!
    echo !C_YELLOW!If LNK1104: close bin\xcat.exe yourself, then re-run build.bat.!C_RESET!
    if exist "bin\xcat_sound.exe" (
        start "" /b "bin\xcat_sound.exe" build-fail >nul 2>&1
    )
    exit /b 1
)

echo.
echo !C_GREEN!Build succeeded: %PRODUCT_NAME% -^> bin\xcat.exe + bin\XCat_data\xcat.dll!C_RESET!
if exist "bin\xcat_sound.exe" (
    start "" /b "bin\xcat_sound.exe" build-ok >nul 2>&1
)

endlocal
