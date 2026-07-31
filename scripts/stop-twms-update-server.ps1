$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Script = Join-Path $Root "scripts\twms-update-server.mjs"

$Processes = Get-CimInstance Win32_Process |
    Where-Object {
        $_.Name -match "node" -and
        $_.CommandLine -and
        $_.CommandLine.Contains("twms-update-server.mjs")
    }

if (-not $Processes) {
    Write-Host "No TWMS update server process found."
    exit 0
}

foreach ($Process in $Processes) {
    Write-Host "Stopping PID $($Process.ProcessId): $($Process.CommandLine)"
    Stop-Process -Id $Process.ProcessId -Force
}
