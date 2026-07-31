param(
    [Parameter(Mandatory = $true)]
    [string]$Package
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path
$Repo = Split-Path -Parent $Root
$Downloads = Join-Path $Root "downloads"
$UpdateDir = Join-Path $Root "update"
$LatestJsonPath = Join-Path $Repo "artifacts\release\latest.json"

if (-not (Test-Path $Package)) {
    throw "Package not found: $Package"
}

$source = Resolve-Path $Package
$name = Split-Path $source -Leaf
if ($name -notlike "xcat_for_twms*.zip") {
    throw "Expected artifacts/release style zip name, got: $name"
}

New-Item -ItemType Directory -Force $Downloads | Out-Null
New-Item -ItemType Directory -Force $UpdateDir | Out-Null

Get-ChildItem $Downloads -Filter "*.zip" |
    Where-Object { $_.Name -notlike "xcat_for_twms*" } |
    ForEach-Object { Remove-Item -Force $_.FullName }

$dest = Join-Path $Downloads $name
Copy-Item -Force $source $dest

$updateZip = Join-Path $UpdateDir $name
Copy-Item -Force $source $updateZip

if (Test-Path $LatestJsonPath) {
    Copy-Item -Force $LatestJsonPath (Join-Path $UpdateDir "latest.json")
} else {
    throw "Missing latest.json: $LatestJsonPath"
}

& "$Root\rebuild-versions.ps1"

Write-Host "Published downloads: $dest"
Write-Host "Published update:    $updateZip"
Write-Host "Manifest:            $UpdateDir\latest.json"
Write-Host "Start server:        publish_site\start-server.ps1"
