// TWMS Classic — attack_accel（攻击加速）
//
// 启用后周期：
//   - LocalUser+0x118=-1（跳过动作等待）
//   - SecondaryStat+0x1BC=140、+0x1C4=远期 expire（PrepareActionLayer 绝对攻速上限）
// 出刀频率由 simpleCombatAttackIntervalMs（面板「间隔」，默认 50，下限 5）控制。
// 禁止 GA .text E9。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "attack_accel.h"

#include "../../runtime/dbg_log_file.h"
#include "../../runtime/log.h"
#include "../../runtime/managed_main.h"
#include "../ports/attack_input_port.h"
#include "../ports/player_combat_port.h"
#include "../ports/world_port.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace x {
namespace features {
namespace attack_accel {
namespace {

constexpr size_t kOffActionBusy = 0x118;       // LocalUser：-1 = idle
constexpr size_t kOffWmMyUser = 0x28;          // WorldManager → MyUser（SSOT）
constexpr size_t kOffWmSecondaryStat = 0xF0;   // WorldManager → SecondaryStat
constexpr size_t kOffAbsSpeed = 0x1BC;         // ActionSpeed 绝对覆盖（!=0 盖掉 GetActionSpeed）
constexpr size_t kOffAbsSpeedExpire = 0x1C4;   // CheckByTime 配对 t
constexpr int kAbsSpeedMax = 140;              // Prepare clamp [70,140] 上限（种子已验）
constexpr DWORD kRefreshMs = 16;
constexpr DWORD kRebindMs = 2000;
constexpr DWORD kLogMs = 3000;
// BIN：进图/落地瞬间写 busy=-1 + speed=140 → 客服脱同步（与是否开打怪无关）。
constexpr DWORD kLandGraceMs = 400;  // 曾 1500；换图后与 fly no_user 叠感「开闸慢」

std::atomic<bool> gDesired{false};
std::atomic<bool> gWorkerStop{false};
std::atomic<HANDLE> gWorkerThread{nullptr};
HANDLE gLog = INVALID_HANDLE_VALUE;

void WriteLogHandle(HANDLE h, const char* buf, int n) {
    if (h == INVALID_HANDLE_VALUE || n <= 0) return;
    DWORD w = 0;
    WriteFile(h, buf, (DWORD)n, &w, nullptr);
}

void Log(const char* fmt, ...) {
    char body[720];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[800];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u %s\n", st.wHour, st.wMinute, st.wSecond,
                     body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    WriteLogHandle(gLog, buf, n);
    x::runtime::LogI("AtkAccel", "%s", body);
}

std::wstring ModuleDir() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&ModuleDir), &self) ||
        !self)
        return L".";
    wchar_t path[MAX_PATH]{};
    if (!GetModuleFileNameW(self, path, MAX_PATH)) return L".";
    std::wstring s(path);
    const size_t slash = s.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return L".";
    return s.substr(0, slash);
}

void OpenLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"attack_accel.log");
}

int ReadI32(void* obj, size_t off) {
    if (!obj) return 0;
    __try {
        return *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void WriteI32(void* obj, size_t off, int v) {
    if (!obj) return;
    __try {
        *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(obj) + off) = v;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void* ReadPtr(void* obj, size_t off) {
    if (!obj) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(obj) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

bool ProbeSsAlive(void* ss) {
    if (!ss) return false;
    __try {
        if (!*reinterpret_cast<void**>(ss)) return false;
        (void)*reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(ss) + kOffAbsSpeed);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int ExpireOrRemainMs() {
    const DWORD now = GetTickCount();
    const int expire = static_cast<int>(now + 3600u * 1000u);
    return expire > 0 ? expire : 0x3FFFFFFF;
}

void WriteActionSpeed(void* ss, bool on) {
    if (!ss) return;
    if (on) {
        WriteI32(ss, kOffAbsSpeed, kAbsSpeedMax);
        WriteI32(ss, kOffAbsSpeedExpire, ExpireOrRemainMs());
    } else {
        WriteI32(ss, kOffAbsSpeed, 0);
        WriteI32(ss, kOffAbsSpeedExpire, 0);
    }
}

void* ResolveLocalUser() {
    if (x::runtime::managed_main::IsLoginFrozen()) return nullptr;
    x::features::ports::player_combat::CombatCtx ctx{};
    if (!x::features::ports::player_combat::QueryCombatCtx(ctx) || !ctx.ok) return nullptr;
    return ctx.localUser;
}

void* ResolveSecondaryStat() {
    if (!x::features::ports::world::IsPlayReady()) return nullptr;
    void* wm = x::features::ports::world::GetWorldManager();
    if (!wm) return nullptr;
    void* ss = ReadPtr(wm, kOffWmSecondaryStat);
    return ProbeSsAlive(ss) ? ss : nullptr;
}

DWORD WINAPI Worker(LPVOID) {
    OpenLog();
    Log("worker start (clearBusy + ActionSpeed=%d landGrace=%ums)", kAbsSpeedMax,
        (unsigned)kLandGraceMs);
    DWORD lastRebind = 0;
    DWORD lastLog = 0;
    DWORD landGraceUntil = 0;  // PlayReady/换皮后宽限，禁止写忙锁与攻速
    void* lu = nullptr;
    void* ss = nullptr;
    void* lastMyUser = nullptr;
    bool wroteSpeed = false;
    bool wasPlayReady = false;

    while (!gWorkerStop.load(std::memory_order_acquire)) {
        const bool on = gDesired.load(std::memory_order_acquire);
        const DWORD now = GetTickCount();

        if (on) {
            // 换图空窗 / 非玩法：立刻丢掉缓存，禁止继续写旧 LocalUser。
            if (!x::features::ports::world::IsPlayReady()) {
                if (wasPlayReady) {
                    wasPlayReady = false;
                    landGraceUntil = 0;
                    lastMyUser = nullptr;
                    // 离 Play 必须清掉已写的 ActionSpeed，否则短黑屏后 140 粘住易脱同步。
                    if (wroteSpeed) {
                        void* cur = (ss && ProbeSsAlive(ss)) ? ss : ResolveSecondaryStat();
                        if (cur) {
                            WriteActionSpeed(cur, false);
                            Log("play not ready — clear ActionSpeed ss=%p", cur);
                        } else {
                            Log("play not ready — pause writes (no ss to clear)");
                        }
                        wroteSpeed = false;
                    } else {
                        Log("play not ready — pause writes");
                    }
                }
                lu = nullptr;
                ss = nullptr;
                Sleep(kRefreshMs);
                continue;
            }
            if (!wasPlayReady) {
                wasPlayReady = true;
                landGraceUntil = now + kLandGraceMs;
                Log("play ready — land grace %ums", (unsigned)kLandGraceMs);
            }

            void* wm = x::features::ports::world::PeekWorldManager();
            void* myUser = wm ? ReadPtr(wm, kOffWmMyUser) : nullptr;
            // MyUser 变了或暂空：立刻丢旧 lu，并重新开落地宽限。
            if (myUser != lastMyUser) {
                if (lastMyUser || myUser) {
                    landGraceUntil = now + kLandGraceMs;
                    Log("MyUser %p -> %p — land grace %ums", lastMyUser, myUser,
                        (unsigned)kLandGraceMs);
                }
                lastMyUser = myUser;
                lu = nullptr;
                ss = nullptr;
            }
            const bool luStale = lu && (!myUser || lu != myUser);
            if (!lu || !ss || luStale || now - lastRebind >= kRebindMs) {
                if (luStale) Log("LocalUser stale %p -> wm.MyUser=%p, rebind", lu, myUser);
                if (luStale && !myUser) {
                    lu = nullptr;
                } else {
                    lu = ResolveLocalUser();
                }
                ss = ResolveSecondaryStat();
                lastRebind = now;
            } else if (ss && !ProbeSsAlive(ss)) {
                ss = nullptr;
            }

            const bool inGrace = landGraceUntil && static_cast<int>(now - landGraceUntil) < 0;
            // ExternalPause（buffs/timed_keys/遇人）：停写 ActionSpeed，避免施法前摇被 140 拽飞。
            const bool fireHeld = x::features::ports::attack::IsFireSuppressed();
            if (inGrace || !lu || fireHeld) {
                if (fireHeld && wroteSpeed) {
                    void* cur = (ss && ProbeSsAlive(ss)) ? ss : ResolveSecondaryStat();
                    if (cur) {
                        WriteActionSpeed(cur, false);
                        wroteSpeed = false;
                        if (now - lastLog >= kLogMs) {
                            lastLog = now;
                            Log("fire suppressed — clear ActionSpeed ss=%p", cur);
                        }
                    }
                }
                // 无皮 / 落地窗 / 停刀闸：绝不写 busy 或 ActionSpeed。
                if (now - lastLog >= kLogMs) {
                    lastLog = now;
                    Log("hold grace=%d suppress=%d lu=%p ss=%p (no write)", inGrace ? 1 : 0,
                        fireHeld ? 1 : 0, lu, ss);
                }
                Sleep(kRefreshMs);
                continue;
            }

            WriteI32(lu, kOffActionBusy, -1);
            if (ss) {
                WriteActionSpeed(ss, true);
                wroteSpeed = true;
            }

            if (now - lastLog >= kLogMs) {
                lastLog = now;
                Log("on lu=%p busy=%d ss=%p speed=%d exp=%d", lu, ReadI32(lu, kOffActionBusy), ss,
                    ss ? ReadI32(ss, kOffAbsSpeed) : 0, ss ? ReadI32(ss, kOffAbsSpeedExpire) : 0);
            }
            Sleep(kRefreshMs);
        } else {
            if (wroteSpeed && ss) {
                WriteActionSpeed(ss, false);
                Log("off clear ActionSpeed ss=%p", ss);
            } else if (wroteSpeed) {
                // 关开关时 SS 可能已换指针：再解析一次清掉残留
                void* cur = ResolveSecondaryStat();
                if (cur) {
                    WriteActionSpeed(cur, false);
                    Log("off clear ActionSpeed (rebind) ss=%p", cur);
                }
            }
            wroteSpeed = false;
            lu = nullptr;
            ss = nullptr;
            lastMyUser = nullptr;
            wasPlayReady = false;
            landGraceUntil = 0;
            Sleep(50);
        }
    }

    if (wroteSpeed) {
        void* cur = ss && ProbeSsAlive(ss) ? ss : ResolveSecondaryStat();
        if (cur) WriteActionSpeed(cur, false);
    }
    Log("worker stop");
    return 0;
}

}  // namespace

void Init() {
    OpenLog();
    Log("Init");
}

void Shutdown() { StopWorker(); }

void StartWorker() {
    if (gWorkerThread.load()) return;
    gWorkerStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (th) gWorkerThread.store(th, std::memory_order_release);
}

void StopWorker() {
    gWorkerStop.store(true, std::memory_order_release);
    HANDLE th = gWorkerThread.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
}

void SetDesired(bool on) {
    const bool was = gDesired.exchange(on, std::memory_order_acq_rel);
    if (was != on) Log("SetDesired on=%d", on ? 1 : 0);
}

bool IsDesired() { return gDesired.load(std::memory_order_acquire); }

}  // namespace attack_accel
}  // namespace features
}  // namespace x
