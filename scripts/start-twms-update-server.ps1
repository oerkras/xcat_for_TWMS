param(
    [string]$HostBind = "0.0.0.0",
    [int]$Port = 18789
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Script = Join-Path $Root "scripts\twms-update-server.mjs"

Write-Host "XCat TWMS update API"
Write-Host "  root: $Root"
Write-Host "  bind: http://${HostBind}:$Port/twms"
Write-Host "  client default: http://xcat.work:$Port/twms"
Write-Host "  manifest: http://xcat.work:$Port/twms/update/latest.json"
Write-Host "  local probe: http://127.0.0.1:$Port/twms/health"
Write-Host "Stop: Ctrl+C or scripts\stop-twms-update-server.ps1"
Write-Host ""

$fw = Join-Path $Root "publish_site\ensure-firewall.ps1"
if (Test-Path $fw) {
    try { & $fw } catch { Write-Warning "firewall ensure skipped: $($_.Exception.Message)" }
}

Set-Location $Root
node $Script --host $HostBind --port $Port --base-path /twms `
  --release-root artifacts\release `
  --out user_log_uploads `
  --accept-profile twms `
  --access-log artifacts\ops_logs\twms_access.jsonl
