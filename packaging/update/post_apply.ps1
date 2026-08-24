# XCat update hook · post_apply
# 打包路径：XCat_data\update\post_apply.ps1（随 release zip）
#
# 调用时机：新树已落到 FinalDest、关键文件校验之后、拉起 xcat.exe 之前。
# 失败：throw（勿 exit）。
#
# 本文件默认空操作。

param(
    [Parameter(Mandatory = $true)][string]$OldDest,
    [Parameter(Mandatory = $true)][string]$FinalDest,
    [Parameter(Mandatory = $true)][string]$Stage,
    [string]$Work = "",
    [string]$InstallAside = "",
    [string]$InstallMode = "",
    [string]$UserPrefsBak = ""
)

$ErrorActionPreference = "Stop"

# 跨版本捞回：测谎战绩 + GAMA PASS账密直登账号。
#
# 换包时 XCat_data\state 整体丢弃，只还原启动器白名单里那几个偏好文件。
# lie_stats.tsv：按角色累计的测谎战绩，清掉补不回来（BIN d43e77）。
# gp_device_login.dpapi / .json：账密直登「当前账号」；182→183 白名单没带上，换包后要重新粘贴。
#
# 白名单已在 update_client.cpp 补上，但那份脚本由**当前已装的**启动器生成，得等客户装上带
# 白名单的版本之后才生效。这个钩子随新包走、当场就能跑，正好补上中间那一次。两边都在时
# 不覆盖已还原的（白名单还原在前，且它取自 stage 快照，比 aside 更可靠）。
#
# 来源优先级：
#   1) $Work\carry_state（pre_apply 在清盘前从旧树暂存；in-place 也有）
#   2) rename-aside 旧树
# 失败只记一行不 throw：为一个偏好文件中止整个换包不值。
$carryLeaves = @('lie_stats.tsv', 'gp_device_login.dpapi', 'gp_device_login.json')
$srcDirs = New-Object 'System.Collections.Generic.List[string]'
if ($Work) {
    $stash = Join-Path $Work 'carry_state'
    if (Test-Path -LiteralPath $stash -PathType Container) {
        [void]$srcDirs.Add($stash)
    }
}
if ($InstallAside -and (Test-Path -LiteralPath $InstallAside)) {
    [void]$srcDirs.Add((Join-Path $InstallAside 'XCat_data\state'))
}
foreach ($leaf in $carryLeaves) {
    try {
        $dst = Join-Path $FinalDest ('XCat_data\state\' + $leaf)
        if (Test-Path -LiteralPath $dst -PathType Leaf) { continue }
        foreach ($dir in $srcDirs) {
            $src = Join-Path $dir $leaf
            if (-not (Test-Path -LiteralPath $src -PathType Leaf)) { continue }
            New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
            Copy-Item -LiteralPath $src -Destination $dst -Force
            Write-Output ($leaf + " carried over from " + $dir)
            break
        }
    } catch {
        Write-Output ("carry " + $leaf + " failed: " + $_.Exception.Message)
    }
}

Write-Output ("post_apply done final=" + $FinalDest + " mode=" + $InstallMode)
return
