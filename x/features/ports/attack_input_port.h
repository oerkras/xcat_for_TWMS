#pragma once
// attack_input_port — Classic TWMS 自动出刀（唯一真源）
//
// 真源：UserLocal.OnFuncKey(Down/Up, FuncKey)。
// 默认读 A 键（InputSystem.Key.A=15）绑定；空才回退 BasicActionAttack(5/52)。
// BIN：硬编 OnKey(Ctrl) 易假成功且 pktSum=0。
// 朝向 = VecCtrl.SetInput(±1,0) Resolve（禁 L/R OnKey Up SEH）。
// 拟人走路：默认内部输入（InputSystem Keyboard 设备状态，见 unity_kbd_port）。

#include <Windows.h>
#include <cstdint>

namespace x::features::ports::attack {

void Init();
void Shutdown();

void SetAttackVk(WORD vk);
WORD GetAttackVk();

void SetIntervalMs(DWORD ms);
DWORD GetIntervalMs();

// 出刀按键 hold（调试 TAB，默认 5ms）。实际 hold = min(此值, 面板间隔)。
// 攻击加速开启时走 Down+Up 同泵的 pulse 路径（hold=0），此值不参与。
void SetAttackHoldMs(DWORD ms);
DWORD GetAttackHoldMs();

void SetSmartInterval(bool on);
bool GetSmartInterval();

// 记录目标相对 dx；出刀前 ApplyFaceNow → VecCtrl.SetInput(±1,0) Resolve 朝向。
bool FaceToward(float dx);
bool ApplyFaceNow();

// 拟人走路：默认灌 Keyboard 设备状态（失焦可走）；出刀/关 F5 前须 StopWalk。inputX 仅 -1/0/1。
// XCAT_WALK_KBD=0 回落 Win32 SendInput（需前台）；XCAT_WALK_KP=1 = PackBit 试验（证伪）。
bool HoldWalk(int inputX);
bool StopWalk();
bool IsWalkHeld();

// 间隔+松键门控后 OnFuncKey（A 槽绑定优先）。
// 软拒绝（间隔未到 / pendingUp / FireSuppressed）返回 false 且不计 fail；仅 OnFuncKey Down 失败计 fail。
bool TryFirePrimary();

// 先泵松键，再看间隔/pending/suppressed；未就绪时勿进 Firing，避免同 tick 空点刷 soft fail。
bool CanFirePrimary();

// Down 未 Up，或距上次成功出刀仍在 recover 窗内。
bool MotionBusy();

// buffs / timed_keys / ExternalPause：禁止新出刀；on 时 ForceRelease。
void SetFireSuppressed(bool on);
bool IsFireSuppressed();

// Hold 后短等：松键完成 + 距上次出刀 ≥ settleMs（默认 32），超时仍返回（不堵死施法）。
bool WaitFireIdle(DWORD timeoutMs, DWORD settleAfterFireMs = 32);

// <0 恢复默认 animBusy；>=0 覆盖前摇占用（攻击加速用）。
void SetAnimBusyOverrideMs(int ms);

// 攻击加速：Down 后同主线程立刻 Up（无 pending 跨 tick）；关闭则恢复 hold 异步松键。
void SetImmediateUp(bool on);

void TickReleases();
void ForceRelease();

// 必须已在主线程泵上（禁止再 Invoke）。SetInput(0,0) 清走路锁存。
// fill+Doing / Settling 自愈用；ForceRelease 会再包一层 Invoke。
void ClearWalkLatchMainThread();

}  // namespace x::features::ports::attack
