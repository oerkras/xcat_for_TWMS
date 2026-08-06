@echo off
setlocal EnableExtensions EnableDelayedExpansion
chcp 65001 >nul 2>&1

:: Classic TWMS / VM 黑屏采证：TDR / DXGI DEVICE_REMOVED / 显示驱动复位
:: 在虚拟机里以管理员运行更完整（事件日志）；普通用户也能跑注册表+显卡信息。

set "OUTDIR="
for /f "usebackq delims=" %%I in (`powershell -NoProfile -Command "Get-Date -Format 'yyyyMMdd_HHmmss'"`) do set "STAMP=%%I"
if not defined STAMP set "STAMP=%RANDOM%"
set "OUTDIR=%~dp0tdr_probe_%COMPUTERNAME%_%STAMP%"
mkdir "%OUTDIR%" 2>nul
set "LOG=%OUTDIR%\tdr_probe.txt"

echo ========================================
echo  TDR / GPU 黑屏采证
echo  输出目录: %OUTDIR%
echo ========================================
echo.

call :log "=== tdr_probe start %DATE% %TIME% ==="
call :log "computer=%COMPUTERNAME% user=%USERNAME%"
call :log "cwd=%CD%"
call :log ""

call :log "=== 1) TDR registry (HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers) ==="
reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v TdrLevel 2>>"%LOG%"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v TdrDelay 2>>"%LOG%"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v TdrDdiDelay 2>>"%LOG%"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v TdrLimitTime 2>>"%LOG%"
reg query "HKLM\SYSTEM\CurrentControlSet\Control\GraphicsDrivers" /v TdrLimitCount 2>>"%LOG%"
call :log "(缺省: TdrLevel=3 启用; TdrDelay=2 秒。未列出=使用系统默认)"
call :log ""

call :log "=== 2) Display adapters (wmic) ==="
wmic path Win32_VideoController get Name,DriverVersion,DriverDate,Status,PNPDeviceID /format:list >>"%LOG%" 2>&1
call :log ""

call :log "=== 3) Recent System log: Display(4101) / nvlddmkm / dxgkrnl / LiveKernel / Watchdog ==="
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='SilentlyContinue';" ^
  "$cut=(Get-Date).AddDays(-3);" ^
  "$ids=@(4101,153,14,219);" ^
  "$ev=@();" ^
  "$ev+=Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$cut;Id=$ids} -MaxEvents 80;" ^
  "$ev+=Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$cut;ProviderName='Microsoft-Windows-Kernel-WHEA'} -MaxEvents 20;" ^
  "$ev+=Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$cut;ProviderName='nvlddmkm'} -MaxEvents 40;" ^
  "$ev+=Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$cut;ProviderName='dxgkrnl'} -MaxEvents 40;" ^
  "$ev+=Get-WinEvent -FilterHashtable @{LogName='System';StartTime=$cut;ProviderName='Display'} -MaxEvents 40;" ^
  "$ev=$ev | Sort-Object TimeCreated -Descending | Select-Object -First 100;" ^
  "if(-not $ev){ 'NO_MATCHING_EVENTS_IN_3_DAYS'; exit 0 };" ^
  "$ev | ForEach-Object {" ^
  "  $msg=($_.Message -replace '\\s+',' ').Substring(0,[Math]::Min(240,($_.Message -replace '\\s+',' ').Length));" ^
  "  '{0:yyyy-MM-dd HH:mm:ss} id={1} src={2} | {3}' -f $_.TimeCreated,$_.Id,$_.ProviderName,$msg" ^
  "}" >>"%LOG%" 2>&1
call :log ""

call :log "=== 4) Application Hang / Maplestory / xcat (last 3 days) ==="
powershell -NoProfile -ExecutionPolicy Bypass -Command ^
  "$ErrorActionPreference='SilentlyContinue';" ^
  "$cut=(Get-Date).AddDays(-3);" ^
  "Get-WinEvent -FilterHashtable @{LogName='Application';StartTime=$cut} -MaxEvents 200 |" ^
  "  Where-Object { $_.Message -match 'Maple|Maplestory|xcat|nvlddmkm|DEVICE_REMOVED|0x887A0005|TDR|Display' } |" ^
  "  Select-Object -First 40 |" ^
  "  ForEach-Object {" ^
  "    $msg=($_.Message -replace '\\s+',' ').Substring(0,[Math]::Min(220,($_.Message -replace '\\s+',' ').Length));" ^
  "    '{0:yyyy-MM-dd HH:mm:ss} id={1} src={2} | {3}' -f $_.TimeCreated,$_.Id,$_.ProviderName,$msg" ^
  "  }" >>"%LOG%" 2>&1
call :log ""

call :log "=== 5) Optional: raise TDR delay (NOT applied — dry-run only) ==="
call :log "若要试验延长 TDR 到 8 秒，管理员 CMD 执行："
call :log "  reg add HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers /v TdrDelay /t REG_DWORD /d 8 /f"
call :log "  reg add HKLM\\SYSTEM\\CurrentControlSet\\Control\\GraphicsDrivers /v TdrDdiDelay /t REG_DWORD /d 8 /f"
call :log "然后重启。恢复默认：删除上述两个值或设回 2。"
call :log ""

call :log "=== done ==="
echo.
echo 完成。请把整个文件夹打包发回：
echo   %OUTDIR%
echo.
explorer "%OUTDIR%"
pause
exit /b 0

:log
echo %~1
>>"%LOG%" echo %~1
exit /b 0
