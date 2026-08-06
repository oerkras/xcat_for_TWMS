$ErrorActionPreference = "Stop"

# TWMS 对外端口（对齐对照仓枫星 ensure-firewall.ps1 模式；产品=经典版）
$rules = @(
    @{ Name = "XCat TWMS API 18789"; Port = 18789 },
    @{ Name = "XCat TWMS Publish HTTP 52080"; Port = 52080 }
)

foreach ($rule in $rules) {
    $existing = Get-NetFirewallRule -DisplayName $rule.Name -ErrorAction SilentlyContinue
    if (-not $existing) {
        New-NetFirewallRule `
            -DisplayName $rule.Name `
            -Direction Inbound `
            -Action Allow `
            -Protocol TCP `
            -LocalPort $rule.Port `
            -Profile Any | Out-Null
        Write-Host "Added firewall rule: $($rule.Name)"
    } else {
        if (-not $existing.Enabled) {
            Enable-NetFirewallRule -DisplayName $rule.Name
            Write-Host "Enabled firewall rule: $($rule.Name)"
        } else {
            Write-Host "Firewall rule exists: $($rule.Name)"
        }
    }
}
