#pragma once
// Shared Unity main-thread dispatcher for Classic TWMS.
// MethodInfo swap on Canvas.SendWillRenderCanvases (fallback: SceneLogin.Update).
// No worker-thread direct invoke — that fatals GC ("Collecting from unknown thread").

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

namespace x::runtime::main_thread {

using JobFn = void (*)(void* user);

// Install pump. Idempotent. Returns false until MI can be patched.
bool Ensure();

// Run fn on Unity main thread. Waits until done/timeout. Never runs on worker.
bool InvokeAndWait(JobFn fn, void* user, DWORD timeoutMs = 1500);

// Sticky per-frame callback: runs on pump thread AFTER orig SendWill/Update.
// No queue, no wait — for light data-plane work only (no GC / no FindAll).
// Pass fn=nullptr to clear. Single slot (last writer wins).
void SetFrameTick(JobFn fn, void* user = nullptr);
bool IsInstalled();

// Poll Ensure() until pump MI is patched or timeout. Safe on worker threads.
// timeoutMs=0 means one shot (same as Ensure). Returns IsInstalled().
bool WaitUntilInstalled(DWORD timeoutMs, DWORD pollMs = 250);

// Always false (direct mode removed). Kept for call-site compatibility.
bool IsDirectMode();

// Restore MethodInfo. Call once on payload unload (probe DETACH).
void Shutdown();

}  // namespace x::runtime::main_thread
