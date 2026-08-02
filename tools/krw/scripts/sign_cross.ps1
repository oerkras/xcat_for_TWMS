#Requires -Version 5.1
<#
.SYNOPSIS
  Cross-sign xcat_krw.sys for loading on Win10 22H2 VMs with Secure Boot OFF
  (no testsigning). Uses expired-but-still-accepted Microsoft cross-cert chain.

.NOTES
  This is NOT attestation/WHQL. Host with Secure Boot ON will still reject (577).
  Needs a commercial-style code signing .pfx whose issuer matches a WDK cross .cer
  (VeriSign / Thawte / GeoTrust). WDKTestCert / Minicat Test Driver will NOT work.

  Env (preferred, keeps secrets out of argv):
    XCAT_KRW_PFX            path to .pfx
    XCAT_KRW_PFX_PASSWORD   pfx password (optional if empty)
    XCAT_KRW_CROSS_CER      optional explicit cross .cer; else auto-pick from WDK
#>
param(
  [string]$SysPath = "",
  [string]$PfxPath = "",
  [string]$PfxPassword = "",
  [string]$CrossCer = "",
  [switch]$SkipTimestamp,
  # Expired leaf certs: temporarily set system clock into cert validity (needs admin).
  [switch]$BackdateForExpired
)

$ErrorActionPreference = "Stop"

function Find-SignTool {
  $kits = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\bin"
  if (-not (Test-Path $kits)) { throw "Windows Kits\10\bin not found" }
  $candidates = Get-ChildItem $kits -Directory | Sort-Object Name -Descending |
    ForEach-Object { Join-Path $_.FullName "x64\signtool.exe" } |
    Where-Object { Test-Path $_ }
  if (-not $candidates) { throw "signtool.exe not found under Windows Kits" }
  return $candidates[0]
}

function Find-CrossCer([string]$preferred) {
  if ($preferred) {
    if (-not (Test-Path -LiteralPath $preferred)) {
      throw "CrossCer not found: $preferred"
    }
    return (Resolve-Path -LiteralPath $preferred).Path
  }
  $root = Join-Path ${env:ProgramFiles(x86)} "Windows Kits\10\CrossCertificates"
  if (-not (Test-Path $root)) { throw "WDK CrossCertificates folder missing: $root" }
  $ver = Get-ChildItem $root -Directory | Sort-Object Name -Descending | Select-Object -First 1
  # Prefer VeriSign Universal / Class3; fall back to any .cer in the folder.
  $names = @(
    "VRSN_C3_PCA_G5_Root_CA_Cross.cer",
    "VRSN_UNIVERSAL_ROOT_CA_cross.cer",
    "GeoTrust_Primary_Root_CA_Cross.cer",
    "GEOTRUST_PCA_G3_CA_cross.cer",
    "Thawte_Primary_Root_CA_Cross.cer",
    "THAWTE_PCA_G3_CA_cross.cer"
  )
  foreach ($n in $names) {
    $p = Join-Path $ver.FullName $n
    if (Test-Path $p) { return $p }
  }
  $any = Get-ChildItem $ver.FullName -Filter *.cer | Select-Object -First 1
  if (-not $any) { throw "No cross .cer under $($ver.FullName)" }
  return $any.FullName
}

if (-not $SysPath) {
  $repo = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..")).Path
  $SysPath = Join-Path $repo "bin\krw\xcat_krw.sys"
}
if (-not $PfxPath) { $PfxPath = $env:XCAT_KRW_PFX }
if (-not $PfxPassword) { $PfxPassword = $env:XCAT_KRW_PFX_PASSWORD }
if (-not $CrossCer) { $CrossCer = $env:XCAT_KRW_CROSS_CER }

if (-not $PfxPath) {
  Write-Host @"
Missing PFX. Set env and re-run:

  `$env:XCAT_KRW_PFX = 'D:\certs\your_codesign.pfx'
  `$env:XCAT_KRW_PFX_PASSWORD = '...'   # if any
  # optional: `$env:XCAT_KRW_CROSS_CER = '...matched_cross.cer'
  .\tools\krw\scripts\sign_cross.ps1

Leaf cert must chain to VeriSign/Thawte/GeoTrust so /ac cross-cert reaches
Microsoft Code Verification Root. Test certs (WDKTestCert / Minicat) cannot.
"@
  exit 2
}

if (-not (Test-Path -LiteralPath $SysPath)) { throw "sys not found: $SysPath" }
if (-not (Test-Path -LiteralPath $PfxPath)) { throw "pfx not found: $PfxPath" }

$signtool = Find-SignTool
$cross = Find-CrossCer $CrossCer
$sys = (Resolve-Path -LiteralPath $SysPath).Path
$pfx = (Resolve-Path -LiteralPath $PfxPath).Path

Write-Host "signtool = $signtool"
Write-Host "cross    = $cross"
Write-Host "pfx      = $pfx"
Write-Host "sys      = $sys"

# Dual sign style used by many legacy kernel packages: SHA256 primary + cross.
$useTimestamp = -not $SkipTimestamp -and -not $BackdateForExpired
$signArgs = @(
  "sign", "/v", "/fd", "sha256",
  "/f", $pfx,
  "/ac", $cross
)
if ($PfxPassword) {
  $signArgs += @("/p", $PfxPassword)
}
if ($useTimestamp) {
  $signArgs += @("/tr", "http://timestamp.digicert.com", "/td", "sha256")
}
$signArgs += $sys

$clockBefore = $null
$w32was = $null
if ($BackdateForExpired) {
  Add-Type -AssemblyName System.Security
  $cert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2(
    $pfx, $PfxPassword,
    [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable)
  $mid = $cert.NotBefore.AddDays(30)
  if ($mid -gt $cert.NotAfter.AddDays(-1)) {
    $mid = $cert.NotBefore.AddHours(1)
  }
  Write-Host "BackdateForExpired: clock -> $mid (leaf NotAfter=$($cert.NotAfter)); timestamp skipped"
  $clockBefore = Get-Date
  $w32was = (Get-Service w32time -ErrorAction SilentlyContinue).Status
  if ($w32was -eq 'Running') {
    Stop-Service w32time -Force -ErrorAction SilentlyContinue
  }
  Set-Date -Date $mid | Out-Null
}

try {
  & $signtool @signArgs
  if ($LASTEXITCODE -ne 0) {
    throw "signtool sign failed ($LASTEXITCODE). Wrong PFX password, expired leaf (try -BackdateForExpired), or cross .cer mismatch."
  }
} finally {
  if ($null -ne $clockBefore) {
    Set-Date -Date $clockBefore -ErrorAction SilentlyContinue | Out-Null
    if ($w32was -eq 'Running') {
      Start-Service w32time -ErrorAction SilentlyContinue
      & w32tm /resync /force 2>$null | Out-Null
    }
    Write-Host "clock restored -> $(Get-Date)"
  }
}

Write-Host "`n=== verify /kp (kernel policy) ==="
& $signtool verify /v /kp $sys
$verifyCode = $LASTEXITCODE
if ($verifyCode -ne 0) {
  Write-Warning "verify /kp returned $verifyCode (0x800B0101 often = CERT_E_EXPIRED on current clock)."
  Write-Warning "On Legacy BIOS / Secure-Boot-unsupported hosts, KMCS may still load if chain reaches Microsoft Code Verification Root."
  if ($BackdateForExpired) {
    Write-Host "Signed OK with -BackdateForExpired; treating expired verify as non-fatal."
    exit 0
  }
  exit $verifyCode
}

Write-Host "OK: cross-signed. Copy/load then smoke or krw_popup.exe."
exit 0
