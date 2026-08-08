<#
.SYNOPSIS
  经典版客户端卡死现场抓取（只读，非侵入）。

.DESCRIPTION
  盯住 Maplestory_Classic.exe 的 Responding 标志。一旦连续无响应超过 -HangSeconds，
  用 cdb 的 **非侵入式附加**（-pv）把各线程 CPU 时间 + 栈顶地址存到文件，然后立刻退出附加。

  为什么是 -pv：非侵入模式不给目标进程设调试端口，IsDebuggerPresent /
  CheckRemoteDebuggerPresent 都看不到；抓完 q 就恢复，也不会在目标退出时把目标带走。

  **能拿到什么**：`!runaway` 的各线程 CPU 时间（两次快照一比，就知道主线程是死等还是
  空转）、每个线程的栈顶 RIP、线程名。ntdll 基址全系统一致，栈顶地址可以拿到任意
  别的进程上用 `ln` 翻译成函数名。

  **拿不到什么**：完整调用栈和模块表。2026-08-09 实测，本客户端的进程对象做过加固，
  即使管理员 + SeDebugPrivilege，cdb 也会报 `ReadVirtual() failed ... (error == 5)`、
  `Peb.Ldr is invalid or inaccessible`，`lm` 输出为空 —— 外部读内存被拒，栈走不下去。
  完整调用栈只能靠进程内的 x::runtime::hang_autopsy（主泵静默即自动回溯全部线程，
  落在 bin/XCat_data/logs/hang/）。本脚本作为它的旁证保留：XCat 自己若也被拖死，
  这条外部通道仍然能证明主线程有没有在烧 CPU。

.EXAMPLE
  powershell -ExecutionPolicy Bypass -File scripts\watch-client-hang.ps1

.NOTES
  脚本只读不写目标进程，也不会结束任何进程；卡死的客户端仍需你自己关。
#>
[CmdletBinding()]
param(
    [string]$ProcessName = 'Maplestory_Classic',
    [int]$HangSeconds = 8,
    [string]$OutDir = "$PSScriptRoot\..\Dumps\runtime\hang",
    # 额外挂微软符号服务器：ntdll/kernel32 帧会更好看，但首次可能要等下载。
    [switch]$MsSymbols
)

$ErrorActionPreference = 'Stop'

function Resolve-Cdb {
    $candidates = @(
        'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe',
        'C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe'
    )
    foreach ($c in $candidates) { if (Test-Path $c) { return $c } }
    $cmd = Get-Command cdb.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

$cdb = Resolve-Cdb
if (-not $cdb) {
    Write-Error "找不到 cdb.exe（需要 Windows SDK 的 Debugging Tools for Windows）。"
    exit 1
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$OutDir = (Resolve-Path $OutDir).Path

$repo = (Resolve-Path "$PSScriptRoot\..").Path
$symParts = @(
    "$repo\build_xcat_for_TWMS\x\probe\Release",
    "$repo\build_xcat_for_TWMS\x\probe\RelWithDebInfo",
    "$repo\bin\XCat_data"
)
if ($MsSymbols) { $symParts += 'srv*C:\symbols*https://msdl.microsoft.com/download/symbols' }
$env:_NT_SYMBOL_PATH = ($symParts -join ';')

Write-Host "cdb      : $cdb"
Write-Host "输出目录 : $OutDir"
Write-Host "监视     : $ProcessName（连续无响应 $HangSeconds 秒即抓栈）"
Write-Host "Ctrl+C 停止。"

$notRespondingSince = $null
$capturedForThisHang = $false
$lastPid = 0

while ($true) {
    $proc = Get-Process -Name $ProcessName -ErrorAction SilentlyContinue | Select-Object -First 1

    if (-not $proc) {
        if ($lastPid -ne 0) { Write-Host "[$(Get-Date -Format HH:mm:ss)] 客户端已退出。" }
        $lastPid = 0; $notRespondingSince = $null; $capturedForThisHang = $false
        Start-Sleep -Seconds 2
        continue
    }

    if ($proc.Id -ne $lastPid) {
        Write-Host "[$(Get-Date -Format HH:mm:ss)] 盯上 pid=$($proc.Id)"
        $lastPid = $proc.Id; $notRespondingSince = $null; $capturedForThisHang = $false
    }

    if ($proc.Responding) {
        if ($notRespondingSince) { Write-Host "[$(Get-Date -Format HH:mm:ss)] 已恢复响应。" }
        $notRespondingSince = $null; $capturedForThisHang = $false
        Start-Sleep -Seconds 1
        continue
    }

    if (-not $notRespondingSince) {
        $notRespondingSince = Get-Date
        Write-Host "[$(Get-Date -Format HH:mm:ss)] 无响应…（满 $HangSeconds 秒才抓，避免误报换图卡顿）"
    }

    $held = ((Get-Date) - $notRespondingSince).TotalSeconds
    if ($held -lt $HangSeconds -or $capturedForThisHang) {
        Start-Sleep -Seconds 1
        continue
    }

    $stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
    $out = Join-Path $OutDir "hang_${stamp}_pid$($proc.Id).txt"
    Write-Host "[$(Get-Date -Format HH:mm:ss)] 抓栈 -> $out"

    # !runaway 先跑：主线程 CPU 时间还在涨 = 死循环；不涨 = 卡在等待上。
    $script = '.echo ===RUNAWAY===; !runaway 7; .echo ===MODULES===; lm; .echo ===MAINTHREAD===; ~0s; k 80; .echo ===ALLTHREADS===; ~*k 30; q'
    try {
        & $cdb -pv -p $proc.Id -c $script 2>&1 | Out-File -FilePath $out -Encoding UTF8
        Write-Host "[$(Get-Date -Format HH:mm:ss)] 已保存（$((Get-Item $out).Length) 字节）。客户端未被结束，你可以自行处理。"
    } catch {
        Write-Warning "抓栈失败：$_"
    }
    $capturedForThisHang = $true
    Start-Sleep -Seconds 2
}
