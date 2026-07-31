@echo off
if defined VSCMD_VER exit /b 0

set "VS2022_ROOT=C:\Program Files\Microsoft Visual Studio\2022"
for %%E in (Community Professional Enterprise BuildTools) do (
  if exist "%VS2022_ROOT%\%%E\Common7\Tools\VsDevCmd.bat" (
    call "%VS2022_ROOT%\%%E\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64
    exit /b 0
  )
)

for /f %%a in ('echo prompt $E ^| cmd') do set "ESC=%%a"
set "C_RED=%ESC%[91m"
set "C_RESET=%ESC%[0m"

echo %C_RED%ERROR: Visual Studio 2022 not found.%C_RESET%
echo Install VS2022 with "Desktop development with C++" and CMake tools.
exit /b 1
