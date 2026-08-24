# XCat update hook · pre_apply
# 打包路径：XCat_data\update\pre_apply.ps1（随 release zip）
#
# 调用时机：stage 校验通过之后、清盘/rename 之前。
# 失败：throw（勿 exit）。
#
# 默认不要 throw：下面只做偏好暂存，单文件失败不中止换包。

param(
    [Parameter(Mandatory = $true)][string]$OldDest,
    [Parameter(Mandatory = $true)][string]$FinalDest,
    [Parameter(Mandatory = $true)][string]$Stage,
    [string]$Work = ""
)

$ErrorActionPreference = "Stop"

# 换包前把跨版本偏好先藏进 $Work\carry_state。
#
# 这份钩子随**新包**走，在旧树还在时执行，不依赖已装启动器白名单。
# 覆盖两条换包路径：
#   - rename-aside：旧树稍后仍可从 aside 捞（post_apply）
#   - in-place：没有 aside，白名单又由旧启动器生成时，只能靠这里
#
# 失败只记一行。
$carryLeaves = @('lie_stats.tsv', 'gp_device_login.dpapi', 'gp_device_login.json')
if ($Work) {
    $stash = Join-Path $Work 'carry_state'
    try {
        New-Item -ItemType Directory -Path $stash -Force | Out-Null
        foreach ($leaf in $carryLeaves) {
            try {
                $src = Join-Path $OldDest ('XCat_data\state\' + $leaf)
                if (Test-Path -LiteralPath $src -PathType Leaf) {
                    Copy-Item -LiteralPath $src -Destination (Join-Path $stash $leaf) -Force
                    Write-Output ($leaf + " stashed from old dest")
                }
            } catch {
                Write-Output ("stash " + $leaf + " failed: " + $_.Exception.Message)
            }
        }
    } catch {
        Write-Output ("carry_state mkdir failed: " + $_.Exception.Message)
    }
} else {
    Write-Output "pre_apply stash skipped (no Work dir)"
}

Write-Output ("pre_apply done old=" + $OldDest + " stage=" + $Stage)
return
