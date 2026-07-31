$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Server = Join-Path $Root "server.py"

$Processes = Get-CimInstance Win32_Process |
    Where-Object {
        $_.Name -match "python" -and
        $_.CommandLine -and
        $_.CommandLine.Contains($Server)
    }

if (-not $Processes) {
    Write-Host "No XCat TWMS publish server process found."
    exit 0
}

foreach ($Process in $Processes) {
    Write-Host "Stopping PID $($Process.ProcessId): $($Process.CommandLine)"
    Stop-Process -Id $Process.ProcessId -Force
}
