#pragma once
// Classic TWMS — 内部输入真源：UnityEngine.InputSystem Keyboard 设备状态。
// 走位是**变化驱动**，不是轮询。已在 IDB 逐个查完：游戏代码（0x7ff849xxxxxx 段）对
// Keyboard.get_current、InputAction.IsPressed/WasPressedThisFrame/ReadValue/triggered、
// Input.GetAxis(Raw)/GetKeyInt 全部**零调用**；唯一的旧版 Input.GetKey(KeyCode) 在
// UserLocal.IsSit()（RVA 0x1080BA0，实读种子算出 273/275/276=上右左）只管起立。
// 配上运行时实测（纯事件注入 71.4% 能走 / 直写状态缓冲降到 10.2%），结论是门闩吃状态**变化**
// （change monitor / InputAction 回调），不看当前状态 —— 所以只有把 StateEvent 灌进队列才算数，
// 直写缓冲反而自断触发。
// 不依赖窗口焦点：事件绕过 Raw Input，直接进 InputSystem 队列。
// 前台卡顿另需 PreProcessEvent 守位钩子压住外来「方向键=未按」事件，见 .cpp 内注释。
//
// Hold 语义：gMask 非空时自管 InputFrameTick 每帧 Repush（不依赖走路 armed）。
// Travel StickUp / timed_keys / autopot 与走路共用同一套持有+补写。
// 全部 OnMain 接口仅限 Unity 主线程；HoldUntil 可在 worker 调（内部 InvokeAndWait）。

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>

namespace x::features::ports::unity_kbd {

// InputSystem.Key 枚举值 == KeyboardState 位号（dump.cs 已核）。
constexpr int32_t kKeyLeftArrow = 61;
constexpr int32_t kKeyRightArrow = 62;
constexpr int32_t kKeyUpArrow = 63;
constexpr int32_t kKeyDownArrow = 64;

// 主线程：把某个 Key 置为按住/松开并立刻入队一帧设备状态。
bool SetKeyHeldOnMain(int32_t unityKey, bool down);

// 主线程：方向锁存，-1 左 / +1 右 / 0 双松（互斥，切向时自动清另一侧）。
bool SetWalkDirOnMain(int inputX);

// 主线程：清空**走路**左右键并入队（不碰 PageDown/技能/StickUp 等脉冲位）。
bool ReleaseAllOnMain();

// 主线程：Hold 租约（与 SetKeyHeld 等价，语义更明确；掩码非空时挂帧 tick）。
bool BeginHoldOnMain(int32_t unityKey);
bool EndHoldOnMain(int32_t unityKey);
bool AnyHeld();
uint32_t HeldMaskHash();

// Worker 安全：Down → 轮询 → 必 Up（失败也松）。
// until(user) 在 worker 调；返回 true 表示可以结束 hold。
// 时机：至少按住 minHoldMs；之后 until 为真或到达 maxHoldMs 再松。
// until 在 minHold 之前为真时仍撑满 minHold（进门 drain）。
// afterUntilDrainMs：until 命中后再额外按住（MapId 晚闪时避免立刻松键）。
using HoldUntilFn = bool (*)(void* user);
bool HoldUntil(int32_t unityKey, DWORD minHoldMs, DWORD maxHoldMs, HoldUntilFn until, void* user,
               char* detail, size_t detailCap, DWORD afterUntilDrainMs = 0);

// 每帧补写（InputFrameTick 槽 / 手动）：只重发当前掩码，绝不绑定 / 不分配 / 不打日志。
// 真实按住的键在设备缓冲里是持续存在的；我们用「绝对状态事件」模拟，就必须每帧补，
// 否则前台的真实键盘事件（方向键=未按）会把注入的状态覆盖掉，表现为一顿一顿。
// 未绑定或手里没按键时直接 no-op。
bool RepushOnMain();

// 累计计数（均为单调累加，调用方取差值得到本区间速率）：
//   pushes   = 已入队的状态事件数（走 QueueEvent，负责按键边沿与 InputAction 回调）
//   clobbers = 被外来键盘事件覆盖的次数（设备 m_LastUpdateTimeInternal 跑到我们写入值之后）
//   directs  = 直写设备前台状态缓冲的次数（XCAT_KBD_DIRECT 才开，已证伪，默认为 0）
//   guards   = 在 Keyboard.PreProcessEvent 里把方向键位补回外来事件的次数
//              ≡ 被拦下的 canceled 数；前台应≈clobbers 率，失焦应≈0
//   hookCalls= 守位钩子被调到的次数。guards=0 时必须靠它区分「钩子没被调」（hookCalls=0，
//              说明派发不走这个 vtable 槽或设备标志位没开）和「被调但没改」（hookCalls>0）
void Stats(uint32_t* pushes, uint32_t* clobbers, uint32_t* directs, uint32_t* guards,
           uint32_t* hookCalls);

// 守位钩子是否已装上（vtable 槽匹配成功）。没装上则前台卡顿修复未生效。
bool GuardActive();

// 已绑定（klass/方法齐；设备在首次 Push 时再取）。
bool Ready();

// 尝试 Bind（可在 worker 调；失败有退避）。Ready() 为真前 Inject 应先 EnsureBound。
bool EnsureBound();

// 最近一次失败原因，供日志；成功为 "ok"。
const char* LastFail();

// 卸载前必须调用：还原 PreProcessEvent 的 vtable 槽并清空掩码。
// 不还原的话，DLL 卸载后游戏下一个键盘事件会调进已释放内存。
void Shutdown();

}  // namespace x::features::ports::unity_kbd
