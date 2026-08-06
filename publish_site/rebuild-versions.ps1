$ErrorActionPreference = "Stop"
$Root = if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    $PSScriptRoot
} else {
    Split-Path -Parent $MyInvocation.MyCommand.Path
}
if ([string]::IsNullOrWhiteSpace($Root)) {
    throw "Cannot resolve publish_site root"
}
$Repo = Split-Path -Parent $Root
$Downloads = Join-Path $Root "downloads"
$LatestJsonPath = Join-Path $Repo "artifacts\release\latest.json"
$VersionsJsonPath = Join-Path $Root "versions.json"

if (-not (Test-Path $Downloads)) {
    New-Item -ItemType Directory -Force $Downloads | Out-Null
}

$latestZipName = $null
if (Test-Path $LatestJsonPath) {
    $latest = Get-Content -Raw $LatestJsonPath | ConvertFrom-Json
    $latestZipName = $latest.zipName
}

$entries = @()
Get-ChildItem $Downloads -Filter "xcat_for_twms*.zip" |
    Sort-Object LastWriteTime -Descending |
    ForEach-Object {
        $sizeMb = [Math]::Round($_.Length / 1MB, 2)
        $entries += [pscustomobject]@{
            name = $_.Name
            file = "/downloads/$($_.Name)"
            publishedAt = $_.LastWriteTime.ToString("yyyy-MM-dd HH:mm:ss")
            sizeBytes = $_.Length
            sizeText = "$sizeMb MB"
            isLatest = ($_.Name -eq $latestZipName)
        }
    }

if (-not ($entries | Where-Object isLatest) -and $entries.Count -gt 0) {
    $entries[0].isLatest = $true
}

$manifest = [ordered]@{
    updatedAt = (Get-Date).ToString("yyyy-MM-dd HH:mm:ss")
    latest = if ($latestZipName) { $latestZipName } else { if ($entries.Count -gt 0) { $entries[0].name } else { "" } }
    versions = @($entries | ForEach-Object {
        [ordered]@{
            name = $_.name
            file = $_.file
            publishedAt = $_.publishedAt
            sizeBytes = $_.sizeBytes
            sizeText = $_.sizeText
            isLatest = [bool]$_.isLatest
        }
    })
}

$json = $manifest | ConvertTo-Json -Depth 6
[System.IO.File]::WriteAllText($VersionsJsonPath, $json, [System.Text.UTF8Encoding]::new($false))
Write-Host "Wrote $($entries.Count) versions to $VersionsJsonPath"
