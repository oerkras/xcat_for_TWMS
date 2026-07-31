param(
    [string]$HostBind = "0.0.0.0",
    [int]$Port = 18789
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Script = Join-Path $Root "scripts\twms-update-server.mjs"

Write-Host "XCat TWMS update API"
Write-Host "  root: $Root"
Write-Host "  url:  http://127.0.0.1:$Port/twms"
Write-Host "  manifest: http://127.0.0.1:$Port/twms/update/latest.json"
Write-Host "Stop: Ctrl+C or scripts\stop-twms-update-server.ps1"
Write-Host ""

Set-Location $Root
node $Script --host $HostBind --port $Port --base-path /twms
