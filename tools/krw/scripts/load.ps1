#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Create + start fixed-name XCatKrw kernel service (lab / research only).
.NOTES
  Exposes service name "XCatKrw" — NOT the stealth path.
  Stealth path: xcat_krw_compat Init() creates SVC_<uuid> on demand.
  Do NOT repeatedly load/unload on a production host (ETW/PMC BSOD risk).
#>
param(
  [string]$SysPath = ""
)

$ErrorActionPreference = "Stop"
$ServiceName = "XCatKrw"

Write-Host "NOTE: fixed service name = non-stealth lab path. Prefer compat Init() for UUID SCM."

if (-not $SysPath) {
  $repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
  $SysPath = Join-Path $repo "bin\krw\xcat_krw.sys"
}

if (-not (Test-Path -LiteralPath $SysPath)) {
  Write-Error "sys not found: $SysPath (build tools/krw first)"
}

$abs = (Resolve-Path -LiteralPath $SysPath).Path
Write-Host "binPath = $abs"

$existing = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
if ($existing) {
  if ($existing.Status -eq "Running") {
    Write-Host "Service already running."
    exit 0
  }
  sc.exe start $ServiceName | Out-Host
  exit $LASTEXITCODE
}

sc.exe create $ServiceName type= kernel binPath= "`"$abs`"" start= demand | Out-Host
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
sc.exe start $ServiceName | Out-Host
exit $LASTEXITCODE
