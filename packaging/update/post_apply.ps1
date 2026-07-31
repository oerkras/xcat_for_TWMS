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

Write-Output ("post_apply noop final=" + $FinalDest + " mode=" + $InstallMode)
return
