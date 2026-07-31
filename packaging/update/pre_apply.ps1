# XCat update hook · pre_apply
# 打包路径：XCat_data\update\pre_apply.ps1（随 release zip）
#
# 调用时机：stage 校验通过之后、清盘/rename 之前。
# 失败：throw（勿 exit）。
#
# 本文件默认空操作。

param(
    [Parameter(Mandatory = $true)][string]$OldDest,
    [Parameter(Mandatory = $true)][string]$FinalDest,
    [Parameter(Mandatory = $true)][string]$Stage,
    [string]$Work = ""
)

$ErrorActionPreference = "Stop"

Write-Output ("pre_apply noop old=" + $OldDest + " stage=" + $Stage)
return
