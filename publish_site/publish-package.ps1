param(
    [Parameter(Mandatory = $true)]
    [string]$Package
)

$ErrorActionPreference = "Stop"
# Prefer $PSScriptRoot; MyInvocation.Path can be empty under `& script.ps1`.
$Root = if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    $PSScriptRoot
} else {
    Split-Path -Parent $MyInvocation.MyCommand.Path
}
if ([string]::IsNullOrWhiteSpace($Root)) {
    throw "Cannot resolve publish_site root (PSScriptRoot/MyInvocation empty)"
}
$Repo = Split-Path -Parent $Root

if (-not (Test-Path -Path $Package)) {
    throw "Package not found: $Package"
}

$srcFile = Get-Item -Path $Package
$zipName = [string]$srcFile.Name
if ($zipName -notlike "xcat_for_twms*.zip") {
    throw "Expected artifacts/release style zip name, got: $zipName"
}

# Web list reads publish_site/downloads; updater reads publish_site/update.
# Keep both in sync. Use string concat for subdirs (safer than Join-Path here).
$webDir = "$Root" + '\downloads'
$apiDir = "$Root" + '\update'
New-Item -ItemType Directory -Force -Path $webDir | Out-Null
New-Item -ItemType Directory -Force -Path $apiDir | Out-Null

Get-ChildItem -Path $webDir -Filter "*.zip" -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -and ($_.Name -notlike "xcat_for_twms*") -and $_.FullName } |
    ForEach-Object { Remove-Item -Force -LiteralPath $_.FullName }

$webZip = "$webDir\$zipName"
$apiZip = "$apiDir\$zipName"
if ([string]::IsNullOrWhiteSpace($webZip) -or -not $webZip.EndsWith($zipName)) {
    throw "webZip path unresolved: '$webZip' (root=$Root name=$zipName)"
}
if ([string]::IsNullOrWhiteSpace($apiZip) -or -not $apiZip.EndsWith($zipName)) {
    throw "apiZip path unresolved: '$apiZip' (root=$Root name=$zipName)"
}

Copy-Item -Force -Path $srcFile.FullName -Destination $webZip
Copy-Item -Force -Path $srcFile.FullName -Destination $apiZip

foreach ($copyPath in @($webZip, $apiZip)) {
    if (-not (Test-Path -Path $copyPath)) {
        throw "Publish copy missing after Copy-Item: $copyPath"
    }
    $copied = Get-Item -Path $copyPath
    if ($copied.Length -ne $srcFile.Length) {
        throw "Publish copy size mismatch: $copyPath ($($copied.Length) != $($srcFile.Length))"
    }
}

$LatestJsonPath = "$Repo\artifacts\release\latest.json"
if (-not (Test-Path -Path $LatestJsonPath)) {
    throw "Missing latest.json: $LatestJsonPath"
}
Copy-Item -Force -Path $LatestJsonPath -Destination "$apiDir\latest.json"

& "$Root\rebuild-versions.ps1"

$versions = Get-Content -Raw -Path "$Root\versions.json" | ConvertFrom-Json
$listed = @($versions.versions | Where-Object { $_.name -eq $zipName })
if ($listed.Count -lt 1) {
    throw "versions.json missing published zip after rebuild: $zipName"
}
if (-not [bool]$listed[0].isLatest) {
    throw "versions.json did not mark $zipName as isLatest (latest field=$($versions.latest))"
}

Write-Host "Published web zip: $webZip"
Write-Host "Published api zip: $apiZip"
Write-Host "Manifest:          $apiDir\latest.json"
Write-Host "Start server:      publish_site\start-server.ps1"
