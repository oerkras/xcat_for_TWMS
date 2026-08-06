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

set STAGING_DIR=%~dp0bin_ops_staging
set STAGING_EXE=%STAGING_DIR%\xcat_ops.exe

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

cmake -S . -B "%BUILD_DIR%" -G "Visual Studio 17 2022" -A x64
if errorlevel 1 (
    echo.
    echo !C_RED!Build FAILED: CMake configure error.!C_RESET!
    exit /b 1
)

echo.
echo !C_YELLOW!Building xcat_ops only ^(xcat_util + imgui; not full xcat_common^).!C_RESET!
echo !C_YELLOW!Close bin_ops\xcat_ops.exe if link fails ^(LNK1104^).!C_RESET!

cmake --build "%BUILD_DIR%" --config %CONFIG% --target xcat_ops
if not errorlevel 1 goto :ok_primary

echo.
echo !C_YELLOW!Primary link failed — building deps then linking to staging only...!C_RESET!
if not exist "%STAGING_DIR%" mkdir "%STAGING_DIR%"

rem Build deps into normal lib dirs first (never redirect OutDir onto util/imgui).
cmake --build "%BUILD_DIR%" --config %CONFIG% --target imgui_lib
if errorlevel 1 goto :fail
cmake --build "%BUILD_DIR%" --config %CONFIG% --target xcat_util
if errorlevel 1 goto :fail

rem Link only the ops project; do not rebuild project references with a hijacked OutDir.
cmake --build "%BUILD_DIR%" --config %CONFIG% --target xcat_ops -- /p:BuildProjectReferences=false /p:OutDir="%STAGING_DIR%\\" /p:TargetName=xcat_ops
if errorlevel 1 goto :fail

if not exist "%STAGING_EXE%" goto :fail

echo.
echo !C_GREEN!Build succeeded ^(staging^): bin_ops_staging\xcat_ops.exe!C_RESET!
echo !C_YELLOW!bin_ops\xcat_ops.exe was locked. Close ops and re-run to refresh bin_ops, or run the staging exe.!C_RESET!
echo !C_GREEN!Deps: imgui_lib + xcat_util ^(process/log/ini^) — no pet_loot/buffs/maps.!C_RESET!
endlocal
exit /b 0

:ok_primary
echo.
echo !C_GREEN!Build succeeded: %PRODUCT_NAME% -^> bin_ops\xcat_ops.exe!C_RESET!
echo !C_GREEN!Deps: imgui_lib + xcat_util ^(process/log/ini^) — no pet_loot/buffs/maps.!C_RESET!
endlocal
exit /b 0

:fail
echo.
echo !C_RED!Build FAILED: xcat_ops compile/link error.!C_RESET!
echo !C_YELLOW!If LNK1104: close bin_ops\xcat_ops.exe, then re-run build_ops.bat.!C_RESET!
endlocal
exit /b 1
