param(
    [int]$Port = 52080
)

$ErrorActionPreference = "Stop"
$Root = Split-Path -Parent $MyInvocation.MyCommand.Path

Write-Host "XCat TWMS publish root: $Root"
Write-Host "Local URL:  http://127.0.0.1:$Port/"
Write-Host "Manifest:   http://127.0.0.1:$Port/update/latest.json"
Write-Host "Stop: press Ctrl+C or close this window"
Write-Host ""

python "$Root\server.py" --host 0.0.0.0 --port $Port --root "$Root"
