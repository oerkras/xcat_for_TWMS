@echo off
setlocal EnableExtensions
chcp 65001 >nul

rem ============================================================
rem  虚拟机内使用：从映射盘 Z: 同步 bin 到本地盘，再启动 XCAT
rem  不要从 Z:\...\xcat.exe 直接跑（共享盘写日志会卡游戏）
rem ============================================================

rem --- 按你的环境改这两行 ---
set "SRC=Z:\Desktop\xcat_for_TWMS\bin"
set "DST=C:\xcat\bin"

rem 同步后是否启动（1=启动，0=只同步）
set "LAUNCH=1"

echo.
echo [sync] %SRC%
echo [  -^>] %DST%
echo.

if not exist "%SRC%\xcat.exe" (
  echo [FAIL] 源目录没有 xcat.exe：%SRC%
  echo        请确认虚拟机已映射 Z:，且本机路径正确。
  pause
  exit /b 1
)

if not exist "%DST%" mkdir "%DST%" 2>nul

rem /E  含子目录  /XO 跳过较旧文件  /R:2 失败重试
rem /NFL /NDL /NJH /NJS /NP  少刷屏
rem 排除运行时日志，避免本机开发日志盖掉虚拟机现场日志
robocopy "%SRC%" "%DST%" /E /XO /R:2 /W:1 /NFL /NDL /NJH /NJS /NP ^
  /XD logs ^
  /XF *.log *.log.* *.jsonl *.jsonl.*

rem robocopy: 0-7 都算成功
set "RC=%ERRORLEVEL%"
if %RC% GEQ 8 (
  echo [FAIL] robocopy 失败，exit=%RC%
  pause
  exit /b %RC%
)

echo [OK] 同步完成。

if not exist "%DST%\xcat.exe" (
  echo [FAIL] 目标没有 xcat.exe：%DST%
  pause
  exit /b 1
)

if "%LAUNCH%"=="1" (
  echo [run] "%DST%\xcat.exe"
  start "" "%DST%\xcat.exe"
) else (
  echo [skip] 未启动（LAUNCH=0）
)

endlocal
exit /b 0
