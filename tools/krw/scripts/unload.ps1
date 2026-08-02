#Requires -RunAsAdministrator
<#
.SYNOPSIS
  Stop + delete XCatKrw lab service; optionally stop drivers whose ImagePath matches xcat_krw.sys.
.NOTES
  Fixed name "XCatKrw" is the lab path. Stealth loads use SVC_<uuid> — use -ByImagePath to find them.
#>
param(
  [switch]$ByImagePath,
  [string]$SysPath = ""
)

$ErrorActionPreference = "Continue"

function Stop-DeleteService([string]$Name) {
  $existing = Get-Service -Name $Name -ErrorAction SilentlyContinue
  if (-not $existing) { return }
  if ($existing.Status -eq "Running") {
    sc.exe stop $Name | Out-Host
    Start-Sleep -Seconds 1
  }
  sc.exe delete $Name | Out-Host
}

# Lab fixed name
Stop-DeleteService "XCatKrw"

if ($ByImagePath) {
  if (-not $SysPath) {
    $repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
    $SysPath = Join-Path $repo "bin\krw\xcat_krw.sys"
  }
  $abs = $null
  if (Test-Path -LiteralPath $SysPath) {
    $abs = (Resolve-Path -LiteralPath $SysPath).Path.ToLowerInvariant()
  }
  Get-ChildItem "HKLM:\SYSTEM\CurrentControlSet\Services" | ForEach-Object {
    $img = (Get-ItemProperty $_.PSPath -Name ImagePath -ErrorAction SilentlyContinue).ImagePath
    if (-not $img) { return }
    $norm = $img.Trim('"').ToLowerInvariant()
    $hit = $norm -like "*xcat_krw.sys*"
    if ($abs -and $norm -eq $abs) { $hit = $true }
    if ($hit) {
      $name = $_.PSChildName
      if ($name -eq "XCatKrw") { return }
      Write-Host "Matched ImagePath service: $name ($img)"
      Stop-DeleteService $name
    }
  }
}

exit 0
