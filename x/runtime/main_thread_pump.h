#pragma once
// Shared Unity main-thread dispatcher for Classic TWMS.
//
// Bootstrap: Canvas.SendWillRenderCanvases + SceneLogin.Update (budgeted Drain).
// InMap (PlayReady): WorldManager.FixedUpdate hosts Drain (peer sample);
//   WM.Update is secondary (timeScale=0). SendWill keeps FrameTick always.
// Resolve: klass = shape (WM/SceneLogin); MI = Unity name → FindMethodCached(RVA+void0) → RVA.
// Safety: if WM drain-host is idle too long, SendWill temporarily resumes Drain
//   (no fake heartbeat). Leave-map must SetPumpPhase(Bootstrap).
// No worker-thread direct invoke — GC fatal. Features must NOT install a second
// SendWill MethodInfo pump.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include <cstdint>

namespace x::runtime::main_thread {

using JobFn = void (*)(void* user);

// Bootstrap = login/load (SendWill+SceneLogin may Drain).
// InMap = PlayReady: prefer WM.FixedUpdate/Update Drain; falls back to Bootstrap
//   hooks if WM MI missing or WM host idle (see kWmIdleFallbackMs in .cpp).
enum class PumpPhase : int { Bootstrap = 0, InMap = 1 };

// Drain picks higher prio first within the per-tick budget (combat before loot).
enum class JobPrio : int { Low = -1, Normal = 0, High = 1 };

// Install pump. Idempotent. Returns false until MI can be patched.
bool Ensure();

// Run fn on Unity main thread. Waits until done/timeout. Never runs on worker.
// prio: High = 出刀/瞬移；Low = 吸物；默认 Normal。同 tick 预算内先跑更高优。
bool InvokeAndWait(JobFn fn, void* user, DWORD timeoutMs = 1500,
                   JobPrio prio = JobPrio::Normal);

// Sticky per-frame callback: runs on pump thread AFTER orig SendWill (always).
// No queue, no wait — for light data-plane work only (no GC / no FindAll).
// Primary slot (invuln anti-blink); nullptr clears it.
void SetFrameTick(JobFn fn, void* user = nullptr);
// Second sticky slot (legacy / 非物理路径); 与 primary 并存，互不覆盖。
void SetAuxFrameTick(JobFn fn, void* user = nullptr);
// 第三槽：只读 walk BIN（keypad_walk_bin）；与 primary/aux 并存。
void SetBinFrameTick(JobFn fn, void* user = nullptr);
// 第四槽：测谎 NonFinite 物理光标脉冲（anti_macro_follower）；与上三者并存。
void SetLieFrameTick(JobFn fn, void* user = nullptr);
// 物理前槽：WM.FixedUpdate 的 orig 之前调用（拟人走路 SetInput，赶在 CalcWalk 前）。
// WM FixedUpdate 未挂上时不会触发；与 SendWill FrameTick 无关。
void SetPrePhysicsFrameTick(JobFn fn, void* user = nullptr);
// 物理后槽：WM.FixedUpdate orig **之后**（UserWU 已把 InputX 灌成 0 后，再补一轮
// SetInput(held)+基类 CalcWalk）。虚表钩未命中时的保底通道。
void SetPostPhysicsFrameTick(JobFn fn, void* user = nullptr);
// 输入补写槽：与 Pre/PostPhysics 不同，**保证有宿主**。首选 WM.FixedUpdate orig 之前
// （设备状态先就位，本帧读输入的逻辑才看得到）；该钩未挂上或 idle 超过 1s 时自动
// 回落到 SendWill 渲染帧。给 InputSystem 设备状态注入这种「必须每帧补」的用途。
void SetInputFrameTick(JobFn fn, void* user = nullptr);
// 累计触发次数：调用方据此证明补写真的在跑，而不是只注册成功。
uint32_t InputFrameTickRuns();
// 当前宿主：0=未触发 1=WM.FixedUpdate 2=SendWill 保底。
uint8_t InputFrameTickHost();
bool IsInstalled();

void SetPumpPhase(PumpPhase phase);
PumpPhase GetPumpPhase();
// True when Drain is currently hosted by WM hooks (not in idle-fallback).
bool IsWmDrainHostActive();

// Real Unity ticks from the current Drain host. Install does NOT count.
uint32_t RealTickCount();
// ms since last real tick; ~0xFFFFFFFF if never ticked.
DWORD LastRealTickAgeMs();
// True when a real tick landed within maxAgeMs.
bool IsPumpTicking(DWORD maxAgeMs = 1500);

// Poll Ensure() until pump MI is patched or timeout. Safe on worker threads.
// timeoutMs=0 means one shot (same as Ensure). Returns IsInstalled().
bool WaitUntilInstalled(DWORD timeoutMs, DWORD pollMs = 250);

// Always false (direct mode removed). Kept for call-site compatibility.
bool IsDirectMode();

// True when the calling thread is the Unity pump thread (hook/Drain host).
// Use to refuse managed-object writes from workers; do NOT nest InvokeAndWait
// when already true (queued job is already on pump).
bool IsOnPumpThread();

// Unity pump 宿主线程 id；0 = 还没有任何一次 hook 进入过。
// 给 hang_autopsy 用：卡死取证要在一堆线程里认出主线程是哪个。
DWORD PumpThreadId();

// Jobs currently parked in the queue (0..capacity). Lockless snapshot.
int QueuedJobCount();

// True when the queue is near capacity — high-frequency producers SHOULD skip
// low-value work this tick instead of parking more jobs. Parking on a saturated
// queue only yields job timeout → retry → more load (a load death-spiral that
// also raises GC pressure on the pump thread). Lockless snapshot.
// Also true during InterStage quiesce (map-transit && !login-freeze) so play
// features back off without waiting for queue depth.
bool IsCongested();

// Congestion queue-depth threshold, runtime-tunable from the panel/config.
// Clamped to [0, queue capacity]; 0 disables backpressure entirely.
void SetCongestionThreshold(int depth);

// Per-tick Drain budget (how many queued jobs to run after each host tick).
// Clamped to [1, queue capacity]; capacity means drain the whole queue.
void SetDrainBudget(int maxJobs);
int DrainBudget();

// play-ready 上升沿：保持 MapTransitBlock / FindAll 冻结，直到泵空闲（min）或超时（max）。
// 冷启动已有 worker 错峰；软重连 worker 早已在跑，靠这把锁挡齐抢。
void ArmPlayReadySettle();
void CancelPlayReadySettle();
bool IsPlayReadySettling();

// Guard for managed / il2cpp access: debug asserts (__debugbreak), release logs
// a throttled warning. Returns IsOnPumpThread(). Call at the top of any function
// that runs managed code so a worker-thread bypass surfaces loudly here instead
// of as a "Collecting from unknown thread" GC fatal popup.
bool AssertOnPumpThread(const char* tag);

// Restore MethodInfo. Call once on payload unload (probe DETACH).
void Shutdown();

}  // namespace x::runtime::main_thread
