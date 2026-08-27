// TWMS Classic — force_trade（实验 TAB，默认关）。
//
// 0820 客户端：UIUserInfo 刷新人物卡时
//   movzx eax, byte [rax+0x38]     ; CharacterStat.level
//   mov ecx, imm32
//   add ecx, [rip+disp]            ; 该 dword 仅 1 处 xref
//   cmp eax, ecx                   ; 解出 15
//   cmovge rcx, rax                ; signed：level >= 15 走放行块
//
// 主路径：改写那颗 global 使 imm+*global==0 → 任意等级 cmovge 成立。
// 同函数另有 4 处 imm+add ecx,[rip]（解出 0/2/3），后面是 CFF jmp，禁止误写。
// 关 / Shutdown restore。禁止 GA .text / E9 / HWBP。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "force_trade.h"

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/anchor_lamps.h"
#include "xcat_payload_control.h"

#include <atomic>
#include <cstdint>
#include <cstring>

namespace x::features::force_trade {
namespace {

using x::runtime::il2cpp::AtRva;

// UIUserInfo 私有刷新（dump TypeDef 596；hash 名即方法名）
constexpr uint32_t kRvaUiUserInfoRefresh = 0x766C10;
constexpr char kUiUserInfoClass[] =
    "d68968347bfea105bc8a9b1a30f27da64e60faf633020cdf5ed2133f33e7356";
constexpr char kHashRefresh[] =
    "c911ecc86b38fd73ae007fdc52b99578a8348fec8e8c6213bac3f1a6e7f63b7";

constexpr uint32_t kLevelGate = 15u;  // 官方解出阈值；换版变了则 shape 拒写
constexpr uint32_t kThreshTarget = 0u;  // 武装后 imm+global；level>=0 恒真
constexpr size_t kScanMax = 0x2C00;     // 方法体 ~0x2AF0
constexpr DWORD kTickMsArmed = 1000;
constexpr DWORD kTickMsOff = 500;
constexpr DWORD kLogMs = 15000;

enum class GateShape : uint8_t {
    Unknown = 0,
    Ok = 1,
    BadConst = 2,
    BadLevel = 3,
    Unreadable = 4,
};

struct GateShapeInfo {
    GateShape shape = GateShape::Unknown;
    uint32_t imm = 0;
    uint32_t* global = nullptr;
};

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

std::atomic<GateShape> gShape{GateShape::Unknown};
std::atomic<bool> gShapeLogged{false};
std::atomic<bool> gDesired{false};
std::atomic<bool> gThreshArmed{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gThreshHits{0};

uint32_t* gThreshGlobal = nullptr;
uint32_t gThreshOrig = 0;
uint32_t gThreshImm = 0;
bool gThreshSaved = false;

MethodInfoHead* gMi = nullptr;
DWORD gLastLog = 0;

bool WriteU32(uint32_t* p, uint32_t v) {
    if (!p) return false;
    DWORD old = 0;
    if (!VirtualProtect(p, sizeof(uint32_t), PAGE_READWRITE, &old)) {
        __try {
            *p = v;
            return true;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    bool ok = false;
    __try {
        *p = v;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(p, sizeof(uint32_t), old, &old);
    return ok;
}

bool ReadU32(const uint32_t* p, uint32_t* out) {
    if (!p || !out) return false;
    __try {
        *out = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsCmpEaxEcx(const uint8_t* p) {
    // 39 C8 = cmp eax, ecx；3B C1 = 同语义另一编码
    return (p[0] == 0x39 && p[1] == 0xC8) || (p[0] == 0x3B && p[1] == 0xC1);
}

bool HasLevelMovzx(const uint8_t* buf, size_t n) {
    // 0F B6 /r disp8=0x38 → movzx r32, byte [r64+0x38]（CharacterStat.level）
    if (!buf || n < 4) return false;
    for (size_t i = 0; i + 4 <= n; ++i) {
        if (buf[i] == 0x0F && buf[i + 1] == 0xB6) {
            const uint8_t modrm = buf[i + 2];
            if ((modrm & 0xC0) == 0x40 && buf[i + 3] == 0x38) return true;
        }
    }
    return false;
}

const void* RefreshFnPtr() {
    if (gMi) {
        __try {
            if (gMi->methodPointer) return gMi->methodPointer;
            if (gMi->virtualMethodPointer) return gMi->virtualMethodPointer;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    return AtRva<const void*>(kRvaUiUserInfoRefresh);
}

bool EnsureRefreshMi() {
    if (gMi && gMi->methodPointer) return true;
    if (!x::runtime::il2cpp::Ensure()) return false;
    void* klass = x::runtime::il2cpp::FindClass("", kUiUserInfoClass);
    if (!klass) klass = x::runtime::il2cpp::FindClass("", "UIUserInfo");
    if (!klass) return false;
    using x::runtime::il2cpp_method::MethodShape;
    using x::runtime::il2cpp_method::TypeKind;
    constexpr MethodShape kSh{0, TypeKind::Void, true, true, {}};
    const auto mr = x::runtime::il2cpp_method::FindMethodResolved(
        klass, kRvaUiUserInfoRefresh, kSh, nullptr, kHashRefresh);
    if (!mr.method) return false;
    gMi = reinterpret_cast<MethodInfoHead*>(mr.method);
    static bool sLogged = false;
    if (!sLogged) {
        sLogged = true;
        x::runtime::LogI("ForceTrade", "UIUserInfo refresh method path=%s rvaHint=0x%X",
                         x::runtime::il2cpp_method::PathName(mr.path), kRvaUiUserInfoRefresh);
    }
    return gMi != nullptr;
}

GateShapeInfo ProbeGateShape(const void* fn) {
    GateShapeInfo out{};
    if (!fn) {
        out.shape = GateShape::Unreadable;
        return out;
    }
    uint8_t buf[kScanMax]{};
    size_t n = 0;
    __try {
        memcpy(buf, fn, sizeof(buf));
        n = sizeof(buf);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out.shape = GateShape::Unreadable;
        return out;
    }

    const bool hasLevel = HasLevelMovzx(buf, n);
    // B9 imm32 ; 03 0D disp32 ; cmp eax,ecx  —— 同函数另 4 处 add 后是 FF25，必须跟 cmp
    for (size_t i = 0; i + 13 <= n; ++i) {
        if (buf[i] != 0xB9 || buf[i + 5] != 0x03 || buf[i + 6] != 0x0D) continue;
        if (!IsCmpEaxEcx(buf + i + 11)) continue;

        uint32_t imm = 0;
        int32_t disp = 0;
        memcpy(&imm, buf + i + 1, 4);
        memcpy(&disp, buf + i + 7, 4);
        const auto rip = reinterpret_cast<uintptr_t>(fn) + i + 11;
        out.imm = imm;
        out.global = reinterpret_cast<uint32_t*>(rip + static_cast<intptr_t>(disp));
        if (!hasLevel) {
            out.shape = GateShape::BadLevel;
            return out;
        }
        out.shape = GateShape::Ok;
        return out;
    }
    out.shape = GateShape::BadConst;
    return out;
}

GateShapeInfo RefreshGateShape(bool forceLog) {
    (void)EnsureRefreshMi();
    const void* fn = RefreshFnPtr();
    GateShapeInfo info = ProbeGateShape(fn);
    if (info.shape != GateShape::Ok) {
        const void* fallback = AtRva<const void*>(kRvaUiUserInfoRefresh);
        if (fallback && fallback != fn) {
            const GateShapeInfo fb = ProbeGateShape(fallback);
            if (fb.shape == GateShape::Ok) info = fb;
        }
    }
    const GateShape prev = gShape.exchange(info.shape);
    if (forceLog || info.shape != prev ||
        (!gShapeLogged.load() && info.shape != GateShape::Unknown)) {
        gShapeLogged.store(true);
        if (info.shape == GateShape::Ok) {
            x::runtime::LogI("ForceTrade",
                             "shape OK UIUserInfo level cmp imm=0x%X global=%p (rva=0x%X)",
                             info.imm, reinterpret_cast<void*>(info.global),
                             kRvaUiUserInfoRefresh);
        } else {
            x::runtime::LogW("ForceTrade",
                             "shape FAIL code=%u — refuse threshold "
                             "(want mov ecx,imm; add ecx,[rip]; cmp eax,ecx + "
                             "movzx [reg+0x38]) rva=0x%X",
                             static_cast<unsigned>(info.shape), kRvaUiUserInfoRefresh);
        }
    }
    return info;
}

void RestoreThreshold() {
    if (!gThreshSaved || !gThreshGlobal) {
        gThreshArmed.store(false);
        return;
    }
    uint32_t cur = 0;
    if (ReadU32(gThreshGlobal, &cur) && cur == gThreshOrig) {
        gThreshArmed.store(false);
        return;
    }
    if (WriteU32(gThreshGlobal, gThreshOrig)) {
        x::runtime::LogI("ForceTrade", "threshold restore global=%p val=0x%X",
                         reinterpret_cast<void*>(gThreshGlobal), gThreshOrig);
    } else {
        x::runtime::LogW("ForceTrade", "threshold restore FAIL global=%p",
                         reinterpret_cast<void*>(gThreshGlobal));
    }
    gThreshArmed.store(false);
}

bool ArmThreshold(const GateShapeInfo& info, DWORD now) {
    if (info.shape != GateShape::Ok || !info.global) return false;

    const uint32_t want = static_cast<uint32_t>((static_cast<uint64_t>(kThreshTarget) -
                                                 static_cast<uint64_t>(info.imm)) &
                                                0xffffffffu);
    const uint32_t official = static_cast<uint32_t>((static_cast<uint64_t>(kLevelGate) -
                                                     static_cast<uint64_t>(info.imm)) &
                                                    0xffffffffu);

    uint32_t cur = 0;
    if (!ReadU32(info.global, &cur)) return false;
    const uint32_t resolved = static_cast<uint32_t>(info.imm + cur);

    // 未武装：必须解出官方 15，否则拒绝写（防误中同函数其它 seed）
    if (cur != want && resolved != kLevelGate) {
        if (!gLastLog || now - gLastLog >= kLogMs) {
            gLastLog = now;
            x::runtime::LogW("ForceTrade",
                             "refuse arm global=%p imm=0x%X cur=0x%X resolved=%u (want %u)",
                             reinterpret_cast<void*>(info.global), info.imm, cur, resolved,
                             kLevelGate);
        }
        gThreshArmed.store(false);
        return false;
    }

    if (!gThreshSaved || gThreshGlobal != info.global || gThreshImm != info.imm) {
        gThreshOrig = (cur == want) ? official : cur;
        gThreshGlobal = info.global;
        gThreshImm = info.imm;
        gThreshSaved = true;
    }

    if (cur == want) {
        gThreshArmed.store(true);
        return true;
    }

    if (!WriteU32(gThreshGlobal, want)) {
        gThreshArmed.store(false);
        return false;
    }
    gThreshHits.fetch_add(1, std::memory_order_relaxed);
    gThreshArmed.store(true);
    if (!gLastLog || now - gLastLog >= kLogMs) {
        gLastLog = now;
        x::runtime::LogI("ForceTrade",
                         "threshold arm global=%p imm=0x%X orig=0x%X want=0x%X "
                         "(imm+want=%u) hits=%u",
                         reinterpret_cast<void*>(gThreshGlobal), gThreshImm, gThreshOrig, want,
                         static_cast<uint32_t>(gThreshImm + want), gThreshHits.load());
    }
    return true;
}

void ReportLamp() {
    const GateShapeInfo info = RefreshGateShape(false);
    if (info.shape == GateShape::BadConst || info.shape == GateShape::BadLevel ||
        info.shape == GateShape::Unreadable) {
        const char* d = info.shape == GateShape::BadLevel     ? "MISS level+0x38"
                        : info.shape == GateShape::BadConst   ? "MISS const"
                                                              : "MISS unreadable";
        x::runtime::anchor_lamps::Set("ForceTrade", x::runtime::anchor_lamps::AnchorLampCode::Miss,
                                      d);
        return;
    }
    if (info.shape == GateShape::Ok && gThreshArmed.load()) {
        x::runtime::anchor_lamps::Set("ForceTrade", x::runtime::anchor_lamps::AnchorLampCode::Ok,
                                      "thresh");
        return;
    }
    if (info.shape == GateShape::Ok) {
        x::runtime::anchor_lamps::Set("ForceTrade",
                                      x::runtime::anchor_lamps::AnchorLampCode::Degraded,
                                      "shape ok, thresh late");
        return;
    }
    x::runtime::anchor_lamps::Set("ForceTrade", x::runtime::anchor_lamps::AnchorLampCode::Unknown,
                                  "pending");
}

bool MaintainThreshold(DWORD now) {
    const GateShapeInfo info = RefreshGateShape(false);
    if (info.shape != GateShape::Ok) {
        gThreshArmed.store(false);
        if (!gLastLog || now - gLastLog >= kLogMs) {
            gLastLog = now;
            ReportLamp();
        }
        return false;
    }
    const bool ok = ArmThreshold(info, now);
    if (!gLastLog || now - gLastLog >= kLogMs) {
        gLastLog = now;
        uint32_t cur = 0;
        (void)ReadU32(info.global, &cur);
        x::runtime::LogI("ForceTrade",
                         "threshold %s global=%p cur=0x%X armed=%d hits=%u",
                         ok ? "ok" : "FAIL", reinterpret_cast<void*>(info.global), cur,
                         gThreshArmed.load() ? 1 : 0, gThreshHits.load());
        ReportLamp();
    }
    return ok;
}

DWORD WINAPI Worker(LPVOID) {
    x::runtime::LogI("ForceTrade", "worker start — UIUserInfo level threshold global (default off)");
    for (int i = 0; i < 400 && !gStop.load() && !GetModuleHandleW(L"GameAssembly.dll"); ++i)
        Sleep(50);
    Tick(GetTickCount());
    while (!gStop.load()) {
        const bool on = gDesired.load();
        Tick(GetTickCount());
        Sleep(on ? kTickMsArmed : kTickMsOff);
    }
    RestoreThreshold();
    x::runtime::LogI("ForceTrade", "worker stop hits=%u", gThreshHits.load());
    return 0;
}

}  // namespace

void Init() {
    gDesired.store(false);
    if (!xcat::kForceTradeUserEnabled) {
        x::runtime::LogI("ForceTrade", "user gate off — skipped (keep code)");
        return;
    }
    x::runtime::LogI("ForceTrade",
                     "init — UIUserInfo trade button level gate (imm+global→0); "
                     "rva=0x%X official=%u (default off)",
                     kRvaUiUserInfoRefresh, kLevelGate);
}

void Shutdown() {
    gDesired.store(false);
    StopWorker();
    RestoreThreshold();
}

void StartWorker() {
    if (!xcat::kForceTradeUserEnabled) {
        gDesired.store(false);
        x::runtime::anchor_lamps::Set("ForceTrade",
                                      x::runtime::anchor_lamps::AnchorLampCode::Unknown,
                                      "disabled");
        return;
    }
    if (gWorker.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (th) gWorker.store(th);
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gWorker.exchange(nullptr);
    if (th) CloseHandle(th);
}

void SetEnabled(bool on) {
    if (!xcat::kForceTradeUserEnabled) on = false;
    const bool prev = gDesired.exchange(on);
    if (prev != on) {
        x::runtime::LogI("ForceTrade", "SetEnabled %d", on ? 1 : 0);
    }
    if (!on) RestoreThreshold();
}

bool IsEnabled() { return gDesired.load(); }

bool IsInstalled() { return gThreshArmed.load(); }

void Tick(DWORD now) {
    if (!gDesired.load()) {
        RestoreThreshold();
        return;
    }
    (void)MaintainThreshold(now);
}

}  // namespace x::features::force_trade
