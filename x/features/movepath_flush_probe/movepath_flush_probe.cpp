// 经典版 / TWMS · MovePath.Flush 采证钩（调试专用，默认关）。见头文件红线说明。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "movepath_flush_probe.h"

#include "../ports/world_port.h"
#include "../../runtime/bin_dir.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <Windows.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x::features::movepath_flush_probe {
namespace {

// MovePath.Flush 的 .text 入口 RVA。2026-08-06 晚 remount：旧 0x119F980 +0x1E70；
// 当前 IDB imagebase 0x7FF848C80000 → VA 0x7FF849E217F0（序言已核对：8×push + sub rsp,4B8h）。
// 字段偏移未漂；序言签名拒钩兜底，客户端再漂会 refuse 而不是瞎 patch。
constexpr uint32_t kRvaMovePathFlush = 0x11BC0A0;

// 序言签名（前 15 字节，到 sub rsp 的 mod/rm+imm8 头）。客户端改版漂移即拒绝下钩，
// 避免 RVA 对不上时把 abs-jmp 覆到随机 .text 上崩游戏。
constexpr uint8_t kFlushSig[15] = {0x55, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
                                   0x54, 0x56, 0x57, 0x53, 0x48, 0x81, 0xEC};
// steal 必须落在指令边界：8×push=12B，sub rsp,imm32=7B → 首个 ≥14 的边界=19B。
constexpr size_t kFlushSteal = 19;

// MovePath 实例字段（IL2CPP object-relative；dumps/move_elem_notes.md 实锤）。
constexpr size_t kOffMpX = 0x10;            // short _x
constexpr size_t kOffMpY = 0x12;            // short _y
constexpr size_t kOffMpVx = 0x14;           // int  _vx
constexpr size_t kOffMpVy = 0x18;           // int  _vy
constexpr size_t kOffMpFhLast = 0x28;       // short _fhLast
constexpr size_t kOffMpElem = 0x30;         // List<MoveElem>
constexpr size_t kOffMpForcedFlush = 0x48;  // bool _forcedFlush
// MoveElem 实例字段
constexpr size_t kOffElAttr = 0x10;          // byte  MovePathType
constexpr size_t kOffElX = 0x12;             // short
constexpr size_t kOffElY = 0x14;             // short
constexpr size_t kOffElVx = 0x16;            // short
constexpr size_t kOffElVy = 0x18;            // short
constexpr size_t kOffElMoveAction = 0x1A;    // byte
constexpr size_t kOffElFh = 0x1C;            // short  当前 foothold id
constexpr size_t kOffElFhFallStart = 0x1E;   // short
constexpr size_t kOffElElapse = 0x20;        // short  本段耗时(ms)

// §4「合法包络」尺子（docs/features/protocol/移动协议.md §4.2 实测）：
// err=‖Δxy − v·el/1000‖，相邻元素、el>0。地面 fh>0 max~17.7；空中 fh=0 p99~27。
// 越界打 `!!`，便于把「被踢前那一批」和 kick.log 的 TCP 死亡边沿对齐。
constexpr double kErrGroundWarn = 18.0;
constexpr double kErrAirWarn = 27.0;

// Flush(this=MovePath, OutPacket*, bool bFly, MovePath* oPath, MethodInfo*)。返回 bool。
using FnFlush = uint8_t(__fastcall*)(void* self, void* outPkt, uint32_t bFly, void* oPath,
                                     void* mi);

struct AbsHookState {
    void* target = nullptr;
    void* trampoline = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};

std::atomic<bool> gWant{false};
AbsHookState gAbs;
FnFlush gTramp = nullptr;
std::atomic<uint64_t> gSeq{0};

int16_t RdI16(void* p, size_t off) {
    if (!p) return 0;
    __try {
        return *reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
int32_t RdI32(void* p, size_t off) {
    if (!p) return 0;
    __try {
        return *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uint8_t RdU8(void* p, size_t off) {
    if (!p) return 0;
    __try {
        return *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
void* RdPtr(void* p, size_t off) {
    if (!p) return nullptr;
    __try {
        return *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(p) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0x48;  // mov rax, imm64
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;  // jmp rax
    p[11] = 0xE0;
}

bool InstallAbs(void* target, void* hook) {
    if (gAbs.active) return true;
    if (!target || !hook) return false;
    for (size_t i = 0; i < sizeof(kFlushSig); ++i) {
        if (reinterpret_cast<uint8_t*>(target)[i] != kFlushSig[i]) {
            x::runtime::LogI("MpFlush", "refuse: sig mismatch @%p b0=%02X", target,
                             reinterpret_cast<uint8_t*>(target)[0]);
            return false;
        }
    }
    void* tramp = VirtualAlloc(nullptr, kFlushSteal + 16, MEM_COMMIT | MEM_RESERVE,
                               PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(gAbs.saved, target, kFlushSteal);
    memcpy(tramp, target, kFlushSteal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + kFlushSteal,
                reinterpret_cast<uint8_t*>(target) + kFlushSteal);
    gTramp = reinterpret_cast<FnFlush>(tramp);  // 先就绪，再改 target，杜绝首帧丢包
    DWORD old = 0;
    if (!VirtualProtect(target, kFlushSteal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        gTramp = nullptr;
        return false;
    }
    WriteAbsJmp(target, hook);
    for (size_t i = 14; i < kFlushSteal; ++i) reinterpret_cast<uint8_t*>(target)[i] = 0x90;
    FlushInstructionCache(GetCurrentProcess(), target, kFlushSteal);
    VirtualProtect(target, kFlushSteal, old, &old);
    gAbs.target = target;
    gAbs.trampoline = tramp;
    gAbs.stolen = kFlushSteal;
    gAbs.active = true;
    return true;
}

void RemoveAbs() {
    if (!gAbs.active || !gAbs.target) return;
    DWORD old = 0;
    if (VirtualProtect(gAbs.target, gAbs.stolen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(gAbs.target, gAbs.saved, gAbs.stolen);
        FlushInstructionCache(GetCurrentProcess(), gAbs.target, gAbs.stolen);
        VirtualProtect(gAbs.target, gAbs.stolen, old, &old);
    }
    if (gAbs.trampoline) VirtualFree(gAbs.trampoline, 0, MEM_RELEASE);
    gAbs.trampoline = nullptr;
    gAbs.target = nullptr;
    gAbs.stolen = 0;
    gAbs.active = false;
    gTramp = nullptr;
}

// 运行在 pump 线程（Flush 内）；只读字段 + 拼串 + 落盘，无托管分配、无 GC。
// 每 elem 附 §4 尺子：`a=N!`=非 Normal；`fh=0*`=空中；`err=..!!`=越合法包络。
// flush 头 `maxErr/air/tel/over` 一眼看清「这批上报有没有畸形」。
void DumpFlush(void* mp, uint32_t bFly) {
    if (!mp) return;
    const int16_t mx = RdI16(mp, kOffMpX), my = RdI16(mp, kOffMpY);
    const int32_t mvx = RdI32(mp, kOffMpVx), mvy = RdI32(mp, kOffMpVy);
    const int16_t fhLast = RdI16(mp, kOffMpFhLast);
    const uint8_t forced = RdU8(mp, kOffMpForcedFlush);

    void* list = RdPtr(mp, kOffMpElem);
    void* arr = list ? RdPtr(list, x::runtime::il2cpp_container::OffListItems()) : nullptr;
    int32_t size = list ? RdI32(list, x::runtime::il2cpp_container::OffListSize()) : 0;
    if (size < 0) size = 0;
    if (size > 32) size = 32;
    const size_t dataOff = x::runtime::il2cpp_container::OffArrayData();

    // 先拼 elem 串并统计（err 需要前驱坐标，故一次遍历）。
    char elems[1024];
    int eo = 0;
    int airCnt = 0, telCnt = 0, overCnt = 0;
    double maxErr = -1.0;
    bool havePrev = false;
    int prevX = 0, prevY = 0;
    for (int i = 0; i < size && eo < static_cast<int>(sizeof(elems)) - 128; ++i) {
        void* el = arr ? RdPtr(arr, dataOff + static_cast<size_t>(i) * sizeof(void*)) : nullptr;
        if (!el) continue;
        const uint8_t a = RdU8(el, kOffElAttr);
        const int16_t ex = RdI16(el, kOffElX), ey = RdI16(el, kOffElY);
        const int16_t evx = RdI16(el, kOffElVx), evy = RdI16(el, kOffElVy);
        const uint8_t ma = RdU8(el, kOffElMoveAction);
        const int16_t efh = RdI16(el, kOffElFh);
        const int16_t efs = RdI16(el, kOffElFhFallStart);
        const int16_t eel = RdI16(el, kOffElElapse);
        if (efh == 0) ++airCnt;
        if (a != 0) ++telCnt;

        // err = ‖Δxy − v·el/1000‖ vs 上一元素；仅 el>0 且有前驱才算。
        double err = -1.0;
        bool over = false;
        if (havePrev && eel > 0) {
            const double dx = static_cast<double>(ex) - static_cast<double>(prevX);
            const double dy = static_cast<double>(ey) - static_cast<double>(prevY);
            const double rx = dx - static_cast<double>(evx) * static_cast<double>(eel) / 1000.0;
            const double ry = dy - static_cast<double>(evy) * static_cast<double>(eel) / 1000.0;
            err = std::sqrt(rx * rx + ry * ry);
            if (err > maxErr) maxErr = err;
            over = err > ((efh == 0) ? kErrAirWarn : kErrGroundWarn);
            if (over) ++overCnt;
        }
        prevX = ex;
        prevY = ey;
        havePrev = true;

        int w;
        if (err >= 0.0) {
            w = snprintf(elems + eo, sizeof(elems) - eo,
                         "%s{a=%u%s xy=(%d,%d) v=(%d,%d) ma=%u fh=%d%s fs=%d el=%d err=%.1f%s}",
                         i ? " " : "", a, a ? "!" : "", ex, ey, evx, evy, ma, efh,
                         efh == 0 ? "*" : "", efs, eel, err, over ? "!!" : "");
        } else {
            w = snprintf(elems + eo, sizeof(elems) - eo,
                         "%s{a=%u%s xy=(%d,%d) v=(%d,%d) ma=%u fh=%d%s fs=%d el=%d err=-}",
                         i ? " " : "", a, a ? "!" : "", ex, ey, evx, evy, ma, efh,
                         efh == 0 ? "*" : "", efs, eel);
        }
        if (w <= 0) break;
        eo += w;
    }

    char merr[16];
    if (maxErr >= 0.0)
        snprintf(merr, sizeof(merr), "%.1f", maxErr);
    else {
        merr[0] = '-';
        merr[1] = '\0';
    }

    char body[1280];
    snprintf(body, sizeof(body),
             "flush #%llu bFly=%u forced=%u xy=(%d,%d) v=(%d,%d) fhLast=%d n=%d "
             "maxErr=%s air=%d tel=%d over=%d [%s]",
             static_cast<unsigned long long>(gSeq.fetch_add(1) + 1), bFly, forced, mx, my, mvx, mvy,
             fhLast, size, merr, airCnt, telCnt, overCnt, elems);

    char dir[512];
    snprintf(dir, sizeof(dir), "%slogs", x::runtime::GetBinDir());
    SYSTEMTIME st;
    GetLocalTime(&st);
    char line[1360];
    const int ln = snprintf(line, sizeof(line), "[%02d:%02d:%02d.%03d] %s\r\n", st.wHour, st.wMinute,
                            st.wSecond, st.wMilliseconds, body);
    if (ln > 0) x::runtime::AppendDbgLogA(dir, "movepath_flush.log", line, static_cast<DWORD>(ln));
    x::runtime::LogI("MpFlush", "%s", body);
}

uint8_t __fastcall HookFlush(void* self, void* outPkt, uint32_t bFly, void* oPath, void* mi) {
    __try {
        // InterStage 黑屏窗：Flush 钩上拼串+双写盘会抢主线程；只在 PlayReady 采证。
        if (gWant.load(std::memory_order_relaxed) &&
            x::features::ports::world::IsPlayReady()) {
            DumpFlush(self, bFly);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    const FnFlush o = gTramp;
    if (!o) return 0;
    return o(self, outPkt, bFly, oPath, mi);
}

// 只在 pump 线程执行：此时该线程必不在 Flush 内，改字节对同线程零竞态。
void PumpApply(void*) {
    const bool want = gWant.load(std::memory_order_acquire);
    if (want && !gAbs.active) {
        void* target = x::runtime::il2cpp::AtRva<void*>(kRvaMovePathFlush);
        const bool ok = InstallAbs(target, reinterpret_cast<void*>(&HookFlush));
        x::runtime::LogI("MpFlush", "arm=%d target=%p", ok ? 1 : 0, target);
    } else if (!want && gAbs.active) {
        RemoveAbs();
        x::runtime::LogI("MpFlush", "disarm");
    }
}

}  // namespace

void SetEnabled(bool on) {
    // 勾上即自行放行 .text 补丁。关开关不撤环境变量（与 melee_veto 共用这根旗）。
    if (on) {
        char env[8]{};
        const DWORD n = GetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", env, sizeof(env));
        if (!(n > 0 && env[0] == '1')) {
            if (!SetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", "1")) {
                x::runtime::LogW("MpFlush", "无法设置 XCAT_ALLOW_TEXT_PATCH=1 err=%lu",
                                 GetLastError());
                return;
            }
        }
    }
    const bool prev = gWant.exchange(on, std::memory_order_acq_rel);
    if (prev == on) return;
    x::runtime::il2cpp_container::Ensure();
    if (x::runtime::main_thread::WaitUntilInstalled(0)) {
        if (!x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 3000,
                                                    x::runtime::main_thread::JobPrio::High)) {
            x::runtime::LogI("MpFlush", "pump invoke failed (want=%d) — 进图后再切一次", on ? 1 : 0);
        }
    } else {
        x::runtime::LogI("MpFlush", "pump not ready (want=%d) — 进图后再切一次", on ? 1 : 0);
    }
}

bool IsActive() { return gAbs.active; }

void Shutdown() {
    gWant.store(false, std::memory_order_release);
    if (!gAbs.active) return;
    if (x::runtime::main_thread::IsInstalled() && !x::runtime::main_thread::IsOnPumpThread())
        x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 2000);
    else
        RemoveAbs();
}

}  // namespace x::features::movepath_flush_probe
