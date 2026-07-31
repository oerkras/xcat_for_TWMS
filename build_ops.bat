@echo off
setlocal EnableDelayedExpansion

cd /d "%~dp0"

for /f %%a in ('echo prompt $E ^| cmd') do set "ESC=%%a"
set "C_GREEN=!ESC![92m"
set "C_RED=!ESC![91m"
set "C_YELLOW=!ESC![93m"
set "C_RESET=!ESC![0m"

set PRODUCT_NAME=xcat_ops
set BUILD_DIR=build
set CONFIG=Release
if not "%~1"=="" set CONFIG=%~1

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo.
    echo !C_RED!Build FAILED: CMake configure error.!C_RESET!
    exit /b 1
)

echo.
echo !C_YELLOW!Building xcat_ops only. Close bin_ops\xcat_ops.exe if link fails ^(LNK1104^).!C_RESET!

cmake --build "%BUILD_DIR%" --config %CONFIG% --target xcat_ops
if errorlevel 1 (
    echo.
    echo !C_RED!Build FAILED: xcat_ops compile/link error.!C_RESET!
    echo !C_YELLOW!If LNK1104: manually close bin_ops\xcat_ops.exe, then re-run build_ops.bat.!C_RESET!
    exit /b 1
)

echo.
echo !C_GREEN!Build succeeded: %PRODUCT_NAME% -^> bin_ops\xcat_ops.exe!C_RESET!

endlocal
