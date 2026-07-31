param(
    [string]$Repo = ""
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
if ([string]::IsNullOrWhiteSpace($Repo)) {
    $Repo = Split-Path -Parent $Root
}

$LatestJson = Join-Path $Repo "artifacts\release\latest.json"
if (-not (Test-Path $LatestJson)) {
    throw "Missing latest.json: $LatestJson"
}

$Latest = Get-Content -Raw $LatestJson | ConvertFrom-Json
$ZipPath = Join-Path (Join-Path $Repo "artifacts\release") $Latest.zipName

if (-not (Test-Path $ZipPath)) {
    throw "Missing release zip: $ZipPath"
}

& "$Root\publish-package.ps1" -Package $ZipPath
