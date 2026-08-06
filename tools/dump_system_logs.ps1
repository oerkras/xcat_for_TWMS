# Requires: Windows PowerShell 5+ / Admin recommended
# Dump System/Application event logs + crash clues for VM black-screen diagnosis.
# Encoding: UTF-8 with BOM (saved by this project)

[CmdletBinding()]
param(
    [string]$OutRoot = ""
)

$ErrorActionPreference = "Continue"

function Test-IsAdmin {
    $id = [Security.Principal.WindowsIdentity]::GetCurrent()
    $p = New-Object Security.Principal.WindowsPrincipal($id)
    return $p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
}

$admin = Test-IsAdmin
$stamp = Get-Date -Format "yyyyMMdd_HHmmss"
if ([string]::IsNullOrWhiteSpace($OutRoot)) {
    $OutRoot = Join-Path $env:USERPROFILE ("Desktop\SysLogDump_" + $stamp)
}

$dirs = @(
    $OutRoot,
    (Join-Path $OutRoot "evtx"),
    (Join-Path $OutRoot "text"),
    (Join-Path $OutRoot "crash"),
    (Join-Path $OutRoot "crash\minidump"),
    (Join-Path $OutRoot "crash\user_crashdumps"),
    (Join-Path $OutRoot "extra")
)
foreach ($d in $dirs) {
    New-Item -ItemType Directory -Force -Path $d | Out-Null
}

Write-Host ""
Write-Host "============================================================"
Write-Host ("  Output: " + $OutRoot)
Write-Host ("  Admin : " + $admin)
Write-Host ("  Host  : " + $env:COMPUTERNAME)
Write-Host "============================================================"
Write-Host ""

function Export-Channel {
    param([string]$Channel, [string]$FileName)
    $dest = Join-Path $OutRoot ("evtx\" + $FileName)
    $errFile = Join-Path $OutRoot ("text\wevtutil_" + [IO.Path]::GetFileNameWithoutExtension($FileName) + "_err.txt")
    $p = Start-Process -FilePath "wevtutil.exe" -ArgumentList @("epl", $Channel, $dest) -Wait -PassThru -NoNewWindow -RedirectStandardError $errFile
    if ($p.ExitCode -ne 0) {
        Write-Host ("  [skip] " + $Channel + " (exit " + $p.ExitCode + ")")
    } else {
        Write-Host ("  [ok]   " + $Channel)
    }
}

Write-Host "[1/8] Export .evtx ..."
Export-Channel "System" "System.evtx"
Export-Channel "Application" "Application.evtx"
if ($admin) { Export-Channel "Security" "Security.evtx" }
@(
    @("Microsoft-Windows-Kernel-Power/Operational", "Kernel-Power.evtx"),
    @("Microsoft-Windows-Kernel-EventTracing/Admin", "Kernel-EventTracing.evtx"),
    @("Microsoft-Windows-WindowsUpdateClient/Operational", "WindowsUpdate.evtx"),
    @("Microsoft-Windows-Hyper-V-Worker-Admin", "HyperV-Worker.evtx"),
    @("Microsoft-Windows-Hyper-V-VMMS-Admin", "HyperV-VMMS.evtx"),
    @("Microsoft-Windows-WHEA-Logger/Operational", "WHEA.evtx"),
    @("Microsoft-Windows-WER-SystemErrorReporting/Operational", "WER-SystemError.evtx")
) | ForEach-Object { Export-Channel $_[0] $_[1] }

function Save-WevtText {
    param([string]$LogName, [string]$Query, [string]$OutFile, [int]$Count = 500)
    $dest = Join-Path $OutRoot ("text\" + $OutFile)
    $args = @("qe", $LogName, "/q:$Query", "/f:text", "/c:$Count")
    $out = & wevtutil.exe @args 2>&1 | Out-String
    Set-Content -Path $dest -Value $out -Encoding UTF8
}

Write-Host "[2/8] System Critical/Error (7d) ..."
Save-WevtText "System" "*[System[(Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= 604800000]]]" "System_CriticalError_7d.txt"

Write-Host "[3/8] Application Critical/Error (7d) ..."
Save-WevtText "Application" "*[System[(Level=1 or Level=2) and TimeCreated[timediff(@SystemTime) <= 604800000]]]" "Application_CriticalError_7d.txt"

Write-Host "[4/8] Power / BugCheck related ..."
Save-WevtText "System" "*[System[(EventID=41 or EventID=1074 or EventID=6008 or EventID=6006 or EventID=6005 or EventID=1001) and TimeCreated[timediff(@SystemTime) <= 1209600000]]]" "System_Power_BugCheck_14d.txt" 200
Save-WevtText "System" "*[System[Provider[@Name='Microsoft-Windows-WHEA-Logger'] and TimeCreated[timediff(@SystemTime) <= 1209600000]]]" "WHEA_14d.txt" 200
Save-WevtText "System" "*[System[Provider[@Name='Microsoft-Windows-Kernel-Power'] and TimeCreated[timediff(@SystemTime) <= 1209600000]]]" "KernelPower_14d.txt" 200
Save-WevtText "System" "*[System[Provider[@Name='Application Popup'] and TimeCreated[timediff(@SystemTime) <= 1209600000]]]" "ApplicationPopup_14d.txt" 200

Write-Host "[5/8] PowerShell summaries ..."
$textDir = Join-Path $OutRoot "text"
try {
    Get-WinEvent -FilterHashtable @{ LogName = "System"; Level = 1, 2; StartTime = (Get-Date).AddDays(-14) } -MaxEvents 300 -ErrorAction SilentlyContinue |
        Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message |
        Format-List |
        Out-File -FilePath (Join-Path $textDir "PS_System_Errors_14d.txt") -Encoding utf8

    Get-WinEvent -FilterHashtable @{ LogName = "Application"; Level = 1, 2; StartTime = (Get-Date).AddDays(-14) } -MaxEvents 300 -ErrorAction SilentlyContinue |
        Select-Object TimeCreated, Id, ProviderName, LevelDisplayName, Message |
        Format-List |
        Out-File -FilePath (Join-Path $textDir "PS_Application_Errors_14d.txt") -Encoding utf8

    Get-WinEvent -FilterHashtable @{ LogName = "System"; Id = 41, 1001, 6008, 1074, 6005, 6006; StartTime = (Get-Date).AddDays(-30) } -MaxEvents 100 -ErrorAction SilentlyContinue |
        Select-Object TimeCreated, Id, ProviderName, Message |
        Format-List |
        Out-File -FilePath (Join-Path $textDir "PS_UnexpectedShutdown_30d.txt") -Encoding utf8

    Get-WinEvent -FilterHashtable @{ LogName = "System"; ProviderName = "Application Popup"; StartTime = (Get-Date).AddDays(-14) } -MaxEvents 100 -ErrorAction SilentlyContinue |
        Select-Object TimeCreated, Id, Message |
        Format-List |
        Out-File -FilePath (Join-Path $textDir "PS_ApplicationPopup_14d.txt") -Encoding utf8
} catch {
    $_ | Out-File (Join-Path $textDir "PS_errors.txt") -Encoding utf8
}

Write-Host "[6/8] Crash dumps ..."
$crashDir = Join-Path $OutRoot "crash"
cmd /c "dir /a C:\Windows\Minidump" | Out-File (Join-Path $crashDir "Minidump_dir.txt") -Encoding utf8
cmd /c "dir /a C:\Windows\MEMORY.DMP" | Out-File (Join-Path $crashDir "MEMORY_DMP_info.txt") -Encoding utf8
$userCrash = Join-Path $env:LOCALAPPDATA "CrashDumps"
cmd /c "dir /a `"$userCrash`"" | Out-File (Join-Path $crashDir "User_CrashDumps_dir.txt") -Encoding utf8

if (Test-Path "C:\Windows\Minidump\*.dmp") {
    Copy-Item "C:\Windows\Minidump\*.dmp" (Join-Path $crashDir "minidump") -Force -ErrorAction SilentlyContinue
}
if (Test-Path (Join-Path $userCrash "*.dmp")) {
    Copy-Item (Join-Path $userCrash "*.dmp") (Join-Path $crashDir "user_crashdumps") -Force -ErrorAction SilentlyContinue
}
@"
MEMORY.DMP is NOT copied by default (often multi-GB).
If needed, copy manually: C:\Windows\MEMORY.DMP
"@ | Set-Content (Join-Path $crashDir "NOTE_MEMORY_DMP.txt") -Encoding utf8

Write-Host "[7/8] systeminfo / drivers / OS ..."
$extra = Join-Path $OutRoot "extra"
systeminfo | Out-File (Join-Path $extra "systeminfo.txt") -Encoding utf8
driverquery /v | Out-File (Join-Path $extra "driverquery.txt") -Encoding utf8
try {
    Get-CimInstance Win32_OperatingSystem |
        Select-Object Caption, Version, BuildNumber, LastBootUpTime, LocalDateTime |
        Format-List |
        Out-File (Join-Path $extra "os_boot.txt") -Encoding utf8
    Get-CimInstance Win32_ComputerSystem |
        Select-Object Manufacturer, Model, TotalPhysicalMemory, HypervisorPresent |
        Format-List |
        Out-File (Join-Path $extra "computer.txt") -Encoding utf8
    Get-CimInstance Win32_PageFileUsage -ErrorAction SilentlyContinue |
        Select-Object Name, AllocatedBaseSize, CurrentUsage, PeakUsage |
        Format-List |
        Out-File (Join-Path $extra "pagefile.txt") -Encoding utf8
} catch {
    $_ | Out-File (Join-Path $extra "cim_errors.txt") -Encoding utf8
}

Write-Host "[8/8] README ..."
$readme = @"
SysLogDump export notes
=======================
Stamp : $stamp
Host  : $env:COMPUTERNAME
User  : $env:USERNAME
Admin : $admin

Check first (black screen / freeze):
  1. text\PS_UnexpectedShutdown_30d.txt   Event 41 / 6008 / 1001
  2. text\PS_ApplicationPopup_14d.txt     dwm.exe / commit memory
  3. text\System_CriticalError_7d.txt
  4. text\WHEA_14d.txt
  5. crash\minidump\
  6. evtx\*.evtx

Key Event IDs:
  41   = Kernel-Power unexpected restart
  1001 = BugCheck BSOD
  6008 = previous shutdown was unexpected
  26   = Application Popup (e.g. dwm commit failure)

Zip this whole folder for analysis.
"@
Set-Content -Path (Join-Path $OutRoot "README.txt") -Value $readme -Encoding utf8

Write-Host ""
Write-Host "============================================================"
Write-Host " DONE"
Write-Host (" " + $OutRoot)
Write-Host "============================================================"
Write-Host ""

try { Start-Process explorer.exe $OutRoot } catch {}
