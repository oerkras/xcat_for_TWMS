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
// Single slot (invuln anti-blink); nullptr clears it.
void SetFrameTick(JobFn fn, void* user = nullptr);
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

// Jobs currently parked in the queue (0..capacity). Lockless snapshot.
int QueuedJobCount();

// True when the queue is near capacity — high-frequency producers SHOULD skip
// low-value work this tick instead of parking more jobs. Parking on a saturated
// queue only yields job timeout → retry → more load (a load death-spiral that
// also raises GC pressure on the pump thread). Lockless snapshot.
bool IsCongested();

// Congestion queue-depth threshold, runtime-tunable from the panel/config.
// Clamped to [0, queue capacity]; 0 disables backpressure entirely.
void SetCongestionThreshold(int depth);

// Per-tick Drain budget (how many queued jobs to run after each host tick).
// Clamped to [1, queue capacity]; capacity (=8) means drain the whole queue.
void SetDrainBudget(int maxJobs);
int DrainBudget();

// Guard for managed / il2cpp access: debug asserts (__debugbreak), release logs
// a throttled warning. Returns IsOnPumpThread(). Call at the top of any function
// that runs managed code so a worker-thread bypass surfaces loudly here instead
// of as a "Collecting from unknown thread" GC fatal popup.
bool AssertOnPumpThread(const char* tag);

// Restore MethodInfo. Call once on payload unload (probe DETACH).
void Shutdown();

}  // namespace x::runtime::main_thread
