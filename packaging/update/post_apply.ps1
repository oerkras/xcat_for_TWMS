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

# 跨版本捞回按角色累计的测谎战绩。
#
# 换包时 XCat_data\state 整体丢弃，只还原启动器白名单里那几个偏好文件。lie_stats.tsv 记的是
# 「这个角色历史上遇到过几次测谎、服端判过几次通过」——攒出来的历史，清掉就再也补不回来。
# BIN d43e77：客户 08-03~08-11 更新九次，seen 从 17 被清成 1。
#
# 白名单已在 update_client.cpp 补上，但那份脚本由**当前已装的**启动器生成，得等客户装上带
# 白名单的版本之后才生效。这个钩子随新包走、当场就能跑，正好补上中间那一次。两边都在时
# 不覆盖已还原的（白名单还原在前，且它取自 stage 快照，比 aside 更可靠）。
#
# 失败只记一行不 throw：为一个统计文件中止整个换包不值。
# in-place 覆盖模式下 aside 为空、旧 state 已被清，这里捞不到也属正常，靠白名单兜下一次。
if ($InstallAside -and (Test-Path -LiteralPath $InstallAside)) {
    try {
        $src = Join-Path $InstallAside 'XCat_data\state\lie_stats.tsv'
        $dst = Join-Path $FinalDest 'XCat_data\state\lie_stats.tsv'
        if ((Test-Path -LiteralPath $src -PathType Leaf) -and
            -not (Test-Path -LiteralPath $dst -PathType Leaf)) {
            New-Item -ItemType Directory -Path (Split-Path -Parent $dst) -Force | Out-Null
            Copy-Item -LiteralPath $src -Destination $dst -Force
            Write-Output "lie_stats.tsv carried over from aside install"
        }
    } catch {
        Write-Output ("carry lie_stats.tsv failed: " + $_.Exception.Message)
    }
}

Write-Output ("post_apply done final=" + $FinalDest + " mode=" + $InstallMode)
return
