@echo off
setlocal EnableExtensions
REM ASCII-only CMD header. Chinese Windows CMD parses BAT as GBK; UTF-8 CJK
REM in IF blocks splits powershell.exe into garbage commands like "ip" / ".exe".
set "BAT_FILE=%~f0"
set "BAT_DIR=%~dp0"
net session >nul 2>&1
if errorlevel 1 echo [WARN] Not admin. Right-click this BAT - Run as administrator.
echo Extracting hang dumps and GPU/TDR events...
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -EncodedCommand JABFAHIAcgBvAHIAQQBjAHQAaQBvAG4AUAByAGUAZgBlAHIAZQBuAGMAZQAgAD0AIAAiAFMAdABvAHAAIgAKACQAcABhAHQAaAAgAD0AIAAkAGUAbgB2ADoAQgBBAFQAXwBGAEkATABFAAoAaQBmACAAKAAtAG4AbwB0ACAAJABwAGEAdABoACkAIAB7ACAAdABoAHIAbwB3ACAAIgBCAEEAVABfAEYASQBMAEUAIABlAG4AdgAgAG4AbwB0ACAAcwBlAHQAIgAgAH0ACgAkAHQAIAA9ACAAWwBJAE8ALgBGAGkAbABlAF0AOgA6AFIAZQBhAGQAQQBsAGwAVABlAHgAdAAoACQAcABhAHQAaAAsACAAWwBUAGUAeAB0AC4ARQBuAGMAbwBkAGkAbgBnAF0AOgA6AFUAVABGADgAKQAKACQAdABhAGcAIAA9ACAAIgBfAF8AIgAgACsAIAAiAFAAUwAxAF8AQgBFAEwATwBXACIAIAArACAAIgBfAF8AIgAKACQAaQAgAD0AIAAkAHQALgBJAG4AZABlAHgATwBmACgAJAB0AGEAZwApAAoAaQBmACAAKAAkAGkAIAAtAGwAdAAgADAAKQAgAHsAIAB0AGgAcgBvAHcAIAAiAG0AYQByAGsAZQByACAAbgBvAHQAIABmAG8AdQBuAGQAIgAgAH0ACgAkAHIAZQBzAHQAIAA9ACAAJAB0AC4AUwB1AGIAcwB0AHIAaQBuAGcAKAAkAGkAIAArACAAJAB0AGEAZwAuAEwAZQBuAGcAdABoACkALgBUAHIAaQBtAFMAdABhAHIAdAAoAFsAYwBoAGEAcgBdADEAMwAsACAAWwBjAGgAYQByAF0AMQAwACkACgAkAHMAYgAgAD0AIABbAHMAYwByAGkAcAB0AGIAbABvAGMAawBdADoAOgBDAHIAZQBhAHQAZQAoACQAcgBlAHMAdAApAAoAJgAgACQAcwBiAA==
set "RC=%ERRORLEVEL%"
echo.
if not "%RC%"=="0" echo [DONE] PowerShell exit code %RC%
echo Done. Send the zip on your Desktop.
pause
exit /b %RC%

__PS1_BELOW__
$ErrorActionPreference = 'SilentlyContinue'
$ProgressPreference = 'SilentlyContinue'
$BatDir = $env:BAT_DIR
if (-not $BatDir) { $BatDir = Split-Path $env:BAT_FILE -Parent }

function Out-Log([string]$Path, [string]$Msg) {
    $line = $Msg
    Write-Host $line
    Add-Content -LiteralPath $Path -Value $line -Encoding UTF8
}

$stamp = Get-Date -Format 'yyyyMMdd_HHmmss'
$desktop = [Environment]::GetFolderPath('Desktop')
if (-not $desktop) { $desktop = Join-Path $env:USERPROFILE 'Desktop' }
$folderName = "xcat_hang_tdr_$($env:COMPUTERNAME)_$stamp"
$outDir = Join-Path $desktop $folderName
$zipPath = "$outDir.zip"
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
New-Item -ItemType Directory -Path (Join-Path $outDir 'hang') -Force | Out-Null
$summary = Join-Path $outDir 'summary.txt'

Out-Log $summary "=== xcat hang+TDR extract $stamp ==="
Out-Log $summary "computer=$($env:COMPUTERNAME) user=$($env:USERNAME)"
Out-Log $summary "batDir=$BatDir"
Out-Log $summary ""

$searchRoots = New-Object System.Collections.Generic.List[string]
function Add-Root([string]$p) {
    if (-not $p) { return }
    if (-not (Test-Path -LiteralPath $p)) { return }
    try {
        $full = (Resolve-Path -LiteralPath $p).Path
        if (-not $searchRoots.Contains($full)) { [void]$searchRoots.Add($full) }
    } catch {}
}
foreach ($p in @(
        $BatDir,
        (Join-Path $BatDir '..'),
        (Join-Path $BatDir '..\..'),
        $desktop,
        [Environment]::GetFolderPath('MyDocuments'),
        (Join-Path $env:USERPROFILE 'Downloads'),
        (Join-Path $env:USERPROFILE 'Desktop')
    )) { Add-Root $p }
Get-Process -Name @('xcat','XCat','Maplestory_Classic') -ErrorAction SilentlyContinue | ForEach-Object {
    try {
        $exe = $_.Path
        if ($exe) {
            $dir = Split-Path $exe -Parent
            Add-Root $dir
            Add-Root (Join-Path $dir 'XCat_data')
            Add-Root (Join-Path $dir 'XCat_data\logs\hang')
        }
    } catch {}
}

Out-Log $summary "=== search roots ==="
$searchRoots | ForEach-Object { Out-Log $summary "  $_" }

$hangFiles = @()
foreach ($root in $searchRoots) {
    $found = Get-ChildItem -LiteralPath $root -Filter 'hang_*.txt' -File -Recurse -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -match '^hang_\d{8}_\d{6}\.txt$' -and (
                $_.DirectoryName -match '[\\/]logs[\\/]hang$' -or
                $_.DirectoryName -eq $desktop
            )
        }
    if ($found) { $hangFiles += @($found) }
}
$hangFiles = $hangFiles | Sort-Object FullName -Unique

Out-Log $summary ""
Out-Log $summary "=== hang files $($hangFiles.Count) ==="
$i = 0
foreach ($f in $hangFiles) {
    $i++
    $destName = '{0:D2}_{1}' -f $i, $f.Name
    Copy-Item -LiteralPath $f.FullName -Destination (Join-Path $outDir "hang\$destName") -Force
    Out-Log $summary ("  {0}  {1}  bytes={2}" -f $f.LastWriteTime.ToString('yyyy-MM-dd HH:mm:ss'), $f.FullName, $f.Length)

    $xcatData = Split-Path (Split-Path $f.DirectoryName -Parent) -Parent
    $ini = Join-Path $xcatData 'state\user.ini'
    if (Test-Path -LiteralPath $ini) {
        $iniDest = Join-Path $outDir ("userini_{0}.ini" -f $i)
        if (-not (Test-Path -LiteralPath $iniDest)) {
            Copy-Item -LiteralPath $ini -Destination $iniDest -Force
            Out-Log $summary "  copied user.ini -> $iniDest"
        }
    }
}
if ($hangFiles.Count -eq 0) {
    Out-Log $summary "  NONE. missing XCat_data\logs\hang\hang_*.txt"
    Out-Log $summary "  Inject xcat before freeze; do not delete XCat_data\logs\hang"
}

$gpuPath = Join-Path $outDir 'gpu.txt'
Out-Log $summary ""
Out-Log $summary "=== GPU / TDR registry ==="
$gpuLines = New-Object System.Collections.Generic.List[string]
try {
    Get-CimInstance Win32_VideoController | ForEach-Object {
        [void]$gpuLines.Add(("Name={0}" -f $_.Name))
        [void]$gpuLines.Add(("DriverVersion={0}" -f $_.DriverVersion))
        [void]$gpuLines.Add(("DriverDate={0}" -f $_.DriverDate))
        [void]$gpuLines.Add(("Status={0}" -f $_.Status))
        [void]$gpuLines.Add(("PNPDeviceID={0}" -f $_.PNPDeviceID))
        [void]$gpuLines.Add(("AdapterRAM={0}" -f $_.AdapterRAM))
        [void]$gpuLines.Add("")
    }
} catch {
    [void]$gpuLines.Add("Get-CimInstance failed: $($_.Exception.Message)")
}
foreach ($dll in @('nvwgf2umx.dll','nvoglv64.dll','nvldumdx.dll','d3d11.dll')) {
    $sys = Join-Path $env:SystemRoot "System32\$dll"
    $wow = Join-Path $env:SystemRoot "SysWOW64\$dll"
    [void]$gpuLines.Add(("exists System32\{0}={1}" -f $dll, (Test-Path -LiteralPath $sys)))
    [void]$gpuLines.Add(("exists SysWOW64\{0}={1}" -f $dll, (Test-Path -LiteralPath $wow)))
}
$tdrKey = 'HKLM:\SYSTEM\CurrentControlSet\Control\GraphicsDrivers'
[void]$gpuLines.Add("")
[void]$gpuLines.Add("TDR registry $tdrKey")
try {
    $props = Get-ItemProperty -LiteralPath $tdrKey
    foreach ($n in @('TdrLevel','TdrDelay','TdrDdiDelay','TdrLimitTime','TdrLimitCount')) {
        $v = $props.$n
        if ($null -eq $v) { $v = '(default)' }
        [void]$gpuLines.Add(("  {0}={1}" -f $n, $v))
    }
} catch {
    [void]$gpuLines.Add("  registry read failed: $($_.Exception.Message)")
}
$gpuLines | Set-Content -LiteralPath $gpuPath -Encoding UTF8
Out-Log $summary "  wrote gpu.txt"

$evtPath = Join-Path $outDir 'tdr_events.txt'
Out-Log $summary ""
Out-Log $summary "=== System events (7 days): Display/nvlddmkm/dxgkrnl/4101 ==="
$cut = (Get-Date).AddDays(-7)
$ev = @()
function Add-Events([hashtable]$Filter) {
    try { return @(Get-WinEvent -FilterHashtable $Filter -MaxEvents 80 -ErrorAction SilentlyContinue) }
    catch { return @() }
}
$ev += Add-Events @{ LogName='System'; StartTime=$cut; Id=@(4101,153,14,219) }
$ev += Add-Events @{ LogName='System'; StartTime=$cut; ProviderName='nvlddmkm' }
$ev += Add-Events @{ LogName='System'; StartTime=$cut; ProviderName='dxgkrnl' }
$ev += Add-Events @{ LogName='System'; StartTime=$cut; ProviderName='Display' }
$ev += Add-Events @{ LogName='System'; StartTime=$cut; ProviderName='Microsoft-Windows-Kernel-WHEA' }
$ev += Add-Events @{ LogName='System'; StartTime=$cut; ProviderName='Microsoft-Windows-WER-SystemErrorReporting' }
$lines = New-Object System.Collections.Generic.List[string]
if (-not $ev) {
    [void]$lines.Add('NO_MATCHING_EVENTS_IN_7_DAYS')
    Out-Log $summary "  no matching System events in 7 days"
} else {
    $uniq = $ev | Sort-Object TimeCreated -Descending | Group-Object RecordId | ForEach-Object { $_.Group[0] } | Select-Object -First 120
    Out-Log $summary ("  events={0}" -f @($uniq).Count)
    foreach ($e in $uniq) {
        $msg = ''
        if ($e.Message) { $msg = (($e.Message -replace '\s+', ' ').Trim()) }
        if ($msg.Length -gt 280) { $msg = $msg.Substring(0, 280) }
        $row = '{0:yyyy-MM-dd HH:mm:ss} id={1} src={2} | {3}' -f $e.TimeCreated, $e.Id, $e.ProviderName, $msg
        [void]$lines.Add($row)
        if ($e.ProviderName -match 'nvlddmkm|dxgkrnl|Display' -or $e.Id -eq 4101) {
            Out-Log $summary ("  HIT {0}" -f $row)
        }
    }
}
$lines | Set-Content -LiteralPath $evtPath -Encoding UTF8

$appPath = Join-Path $outDir 'app_hang_events.txt'
$appEv = @()
try {
    $appEv = @(Get-WinEvent -FilterHashtable @{ LogName='Application'; StartTime=$cut } -MaxEvents 250 -ErrorAction SilentlyContinue |
        Where-Object { $_.Message -match 'Maple|Maplestory|xcat|nvlddmkm|DEVICE_REMOVED|0x887A0005|TDR|Display driver' } |
        Select-Object -First 50)
} catch { }
$appLines = New-Object System.Collections.Generic.List[string]
if (-not $appEv) {
    [void]$appLines.Add('NO_MATCHING_APPLICATION_EVENTS')
} else {
    foreach ($e in $appEv) {
        $msg = ''
        if ($e.Message) { $msg = (($e.Message -replace '\s+', ' ').Trim()) }
        if ($msg.Length -gt 260) { $msg = $msg.Substring(0, 260) }
        [void]$appLines.Add(('{0:yyyy-MM-dd HH:mm:ss} id={1} src={2} | {3}' -f $e.TimeCreated, $e.Id, $e.ProviderName, $msg))
    }
}
$appLines | Set-Content -LiteralPath $appPath -Encoding UTF8

if (Get-Command nvidia-smi -ErrorAction SilentlyContinue) {
    try {
        & nvidia-smi | Out-File -FilePath (Join-Path $outDir 'nvidia-smi.txt') -Encoding utf8
        Out-Log $summary "  wrote nvidia-smi.txt"
    } catch {}
}

Out-Log $summary ""
Out-Log $summary "=== zip ==="
if (Test-Path -LiteralPath $zipPath) { Remove-Item -LiteralPath $zipPath -Force }
try {
    Compress-Archive -Path $outDir -DestinationPath $zipPath -Force
    Out-Log $summary "zip=$zipPath"
} catch {
    Out-Log $summary "zip failed: $($_.Exception.Message)"
    Out-Log $summary "zip folder manually: $outDir"
}

Write-Host ""
Write-Host "OK. Send this zip back:"
Write-Host "  $zipPath"
Write-Host ""
if (Test-Path -LiteralPath $zipPath) {
    Start-Process explorer.exe "/select,`"$zipPath`""
} else {
    Start-Process explorer.exe $outDir
}
exit 0
