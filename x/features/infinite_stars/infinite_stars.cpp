// 经典版 TWMS —— 实验·无限飞镖
//
// 主路径：夜使者 4 转 NightlordSpiritJavelin = 4121006（无形镖；台服常称無限飛鏢）。
// 对照仓 dump `Dumps/cms_cw/dump.cs` Skill.NightlordSpiritJavelin；运行时常量同值。
// 勾上后自动维持该 BUFF：服端认了就不扣飞镖（耗蓝）。未学会只走下面的客户端冻数量。
//
// 辅路径（runtime dump `Dumps/runtime/GameAssembly.dll`）：
//   ItemSlotBundle.SetItemNumber  RVA 0x1303C00  `mov [rcx+0x28], dx; ret` + NOP
//   ItemSlotBundle.GetItemNumber  RVA 0x1303C10  `movzx eax, [rcx+0x28]; ret` + NOP
//   ItemSlotBase.ItemId           @0x10
//   ItemSlotBundle.nNumber        ushort @0x28
// 虚调用走 vtable，PatchMethodInfo 拦不到；函数体只有 5 字节，靠 ret 后对齐 NOP
// 凑够 12 字节 abs-jmp。itemId/10000==207 且新值 < 当前 nNumber → Set 直接 return。
// Get 读到 ≤0 时返回 1。现金镖 5021/5022 暂不覆盖。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "infinite_stars.h"

#include "../ports/skill_port.h"
#include "../ports/world_port.h"
#include "../../runtime/anchor_lamps.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../ui/player_vitals.h"
#include "xcat_payload_control.h"

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace x::features::infinite_stars {
namespace {

using x::runtime::il2cpp::LooksLikeHeapPtr;

constexpr uint32_t kRvaSetItemNumber = 0x1303C00;
constexpr uint32_t kRvaGetItemNumber = 0x1303C10;
constexpr int kSkillSpiritJavelin = 4121006;  // NightlordSpiritJavelin
constexpr float kRenewRemainSec = 8.f;
constexpr DWORD kCastGapMs = 3000;
constexpr DWORD kNoSkillLogMs = 30000;
constexpr DWORD kLevelCacheMs = 5000;

// Set: 66 89 51 28 C3 + 7B NOP；Get: 0F B7 41 28 C3 + 7B NOP
constexpr uint8_t kPrologSet[12] = {0x66, 0x89, 0x51, 0x28, 0xC3, 0x66,
                                    0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00};
constexpr uint8_t kPrologGet[12] = {0x0F, 0xB7, 0x41, 0x28, 0xC3, 0x66,
                                    0x66, 0x2E, 0x0F, 0x1F, 0x84, 0x00};
constexpr size_t kSteal = 12;
constexpr DWORD kHeartMs = 5000;

using FnSet = void(__fastcall*)(void* self, int32_t n, void* methodInfo);
using FnGet = int32_t(__fastcall*)(void* self, void* methodInfo);

struct AbsHookState {
    void* target = nullptr;
    void* trampoline = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};

std::atomic<bool> gWant{false};
std::atomic<bool> gHooksWanted{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<bool> gSetRefuse{false};
std::atomic<bool> gGetRefuse{false};
std::atomic<uint32_t> gBlockDec{0};
std::atomic<uint32_t> gFakeGet{0};
std::atomic<uint32_t> gPassSet{0};

AbsHookState gSet{};
AbsHookState gGet{};
FnSet gSetTramp = nullptr;
FnGet gGetTramp = nullptr;

HANDLE gLog = INVALID_HANDLE_VALUE;
std::wstring gLogPath;

int gSkillLv = -1;
DWORD gSkillLvAt = 0;
DWORD gLastCastTry = 0;
DWORD gLastNoSkillLog = 0;
float gLastRemain = 0.f;
std::atomic<uint32_t> gCastOk{0};
std::atomic<uint32_t> gCastFail{0};

bool IsThrowingStar(int32_t itemId) {
    return itemId / 10000 == 207;
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

void OpenBinLog() {
    if (gLog != INVALID_HANDLE_VALUE) return;
    const std::wstring dir = ModuleDir() + L"\\logs";
    CreateDirectoryW(dir.c_str(), nullptr);
    gLogPath = dir + L"\\infinite_stars.log";
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"infinite_stars.log");
}

void BinLog(const char* fmt, ...) {
    OpenBinLog();
    char body[512]{};
    va_list ap;
    va_start(ap, fmt);
    const int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn <= 0) return;

    SYSTEMTIME st{};
    GetLocalTime(&st);
    char line[640]{};
    const int n =
        snprintf(line, sizeof(line), "%02u:%02u:%02u.%03u %s\r\n", st.wHour, st.wMinute,
                 st.wSecond, st.wMilliseconds, body);
    if (n <= 0) return;
    if (gLog != INVALID_HANDLE_VALUE) {
        DWORD w = 0;
        WriteFile(gLog, line, static_cast<DWORD>(n), &w, nullptr);
    } else if (!gLogPath.empty()) {
        x::runtime::AppendDbgLog(gLogPath, line, static_cast<DWORD>(n));
    }
    x::runtime::LogI("InfStars", "%s", body);
}

void ReportLamp(x::runtime::anchor_lamps::AnchorLampCode code, const char* detail) {
    x::runtime::anchor_lamps::Set("InfStars", code, detail);
}

bool ReadI32(void* p, size_t off, int32_t* out) {
    if (!p || !out) return false;
    __try {
        *out = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(p) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU16(void* p, size_t off, uint16_t* out) {
    if (!p || !out) return false;
    __try {
        *out = *reinterpret_cast<uint16_t*>(reinterpret_cast<uint8_t*>(p) + off);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

bool SigMatch(void* target, const uint8_t* sig, size_t n) {
    if (!target) return false;
    for (size_t i = 0; i < n; ++i) {
        if (reinterpret_cast<uint8_t*>(target)[i] != sig[i]) return false;
    }
    return true;
}

bool InstallAbs(AbsHookState& st, void* target, void* hook, const uint8_t* prolog) {
    if (st.active) return true;
    if (!target || !hook) return false;
    if (!SigMatch(target, prolog, kSteal)) return false;
    void* tramp =
        VirtualAlloc(nullptr, kSteal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    memcpy(st.saved, target, kSteal);
    memcpy(tramp, target, kSteal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + kSteal,
                reinterpret_cast<uint8_t*>(target) + kSteal);
    DWORD old = 0;
    if (!VirtualProtect(target, kSteal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        return false;
    }
    WriteAbsJmp(target, hook);
    FlushInstructionCache(GetCurrentProcess(), target, kSteal);
    VirtualProtect(target, kSteal, old, &old);
    st.target = target;
    st.trampoline = tramp;
    st.stolen = kSteal;
    st.active = true;
    return true;
}

void RemoveAbs(AbsHookState& st) {
    if (!st.active || !st.target) return;
    DWORD old = 0;
    if (VirtualProtect(st.target, st.stolen, PAGE_EXECUTE_READWRITE, &old)) {
        memcpy(st.target, st.saved, st.stolen);
        FlushInstructionCache(GetCurrentProcess(), st.target, st.stolen);
        VirtualProtect(st.target, st.stolen, old, &old);
    }
    st.trampoline = nullptr;
    st.target = nullptr;
    st.stolen = 0;
    st.active = false;
}

void __fastcall HookSet(void* self, int32_t n, void* methodInfo) {
    if (gWant.load(std::memory_order_relaxed) && LooksLikeHeapPtr(self)) {
        int32_t id = 0;
        uint16_t cur = 0;
        const size_t offId = x::ui::player::OffSlotItemId();
        const size_t offN = x::ui::player::OffSlotBundleNumber();
        if (ReadI32(self, offId, &id) && IsThrowingStar(id) && ReadU16(self, offN, &cur)) {
            if (n < static_cast<int32_t>(cur)) {
                gBlockDec.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            gPassSet.fetch_add(1, std::memory_order_relaxed);
        }
    }
    const FnSet o = gSetTramp;
    if (o) o(self, n, methodInfo);
}

int32_t __fastcall HookGet(void* self, void* methodInfo) {
    int32_t n = 0;
    const FnGet o = gGetTramp;
    if (o) {
        n = o(self, methodInfo);
    } else if (LooksLikeHeapPtr(self)) {
        uint16_t cur = 0;
        if (ReadU16(self, x::ui::player::OffSlotBundleNumber(), &cur))
            n = static_cast<int32_t>(cur);
    }
    if (gWant.load(std::memory_order_relaxed) && LooksLikeHeapPtr(self) && n <= 0) {
        int32_t id = 0;
        if (ReadI32(self, x::ui::player::OffSlotItemId(), &id) && IsThrowingStar(id)) {
            gFakeGet.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
    }
    return n;
}

bool TryArmOne(AbsHookState& st, std::atomic<bool>& refuse, uint32_t rva, void* hook,
               const char* name, void** outTramp, const uint8_t* prolog) {
    if (st.active || refuse.load(std::memory_order_relaxed)) return st.active;
    void* target = x::runtime::il2cpp::AtRva<void*>(rva);
    if (!SigMatch(target, prolog, kSteal)) {
        refuse.store(true, std::memory_order_relaxed);
        BinLog("%s refuse: prolog mismatch @%p rva=0x%X b0=%02X", name, target, (unsigned)rva,
               target ? reinterpret_cast<uint8_t*>(target)[0] : 0);
        return false;
    }
    if (!InstallAbs(st, target, hook, prolog)) {
        BinLog("%s install failed @%p rva=0x%X", name, target, (unsigned)rva);
        return false;
    }
    if (outTramp) *outTramp = st.trampoline;
    BinLog("%s arm=1 target=%p rva=0x%X", name, target, (unsigned)rva);
    return true;
}

void PumpApply(void*) {
    const bool want = gHooksWanted.load(std::memory_order_acquire);
    if (want) {
        if (!x::runtime::il2cpp::Ensure()) {
            ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "no-il2cpp");
            return;
        }
        void* setTramp = gSetTramp;
        TryArmOne(gSet, gSetRefuse, kRvaSetItemNumber, reinterpret_cast<void*>(&HookSet), "Set",
                  &setTramp, kPrologSet);
        gSetTramp = reinterpret_cast<FnSet>(setTramp);
        void* getTramp = gGetTramp;
        TryArmOne(gGet, gGetRefuse, kRvaGetItemNumber, reinterpret_cast<void*>(&HookGet), "Get",
                  &getTramp, kPrologGet);
        gGetTramp = reinterpret_cast<FnGet>(getTramp);
        const bool ok = gSet.active && gGet.active;
        ReportLamp(ok ? x::runtime::anchor_lamps::AnchorLampCode::Ok
                      : x::runtime::anchor_lamps::AnchorLampCode::Miss,
                   ok ? "armed" : "partial");
    } else if (gSet.active || gGet.active) {
        RemoveAbs(gGet);
        gGetTramp = nullptr;
        RemoveAbs(gSet);
        gSetTramp = nullptr;
        ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "off");
        BinLog("disarm blockDec=%u fakeGet=%u passSet=%u", gBlockDec.load(), gFakeGet.load(),
               gPassSet.load());
    }
}

// 勾上即自行放行 .text 补丁。关开关不撤环境变量（与 melee_veto 共用这根旗）。
bool EnsurePatchEnv() {
    char env[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", env, sizeof(env));
    if (n > 0 && env[0] == '1') return true;
    if (!SetEnvironmentVariableA("XCAT_ALLOW_TEXT_PATCH", "1")) {
        BinLog("无法设置 XCAT_ALLOW_TEXT_PATCH=1 err=%lu", GetLastError());
        return false;
    }
    return true;
}

void RequestApply() {
    if (!x::runtime::main_thread::WaitUntilInstalled(0)) return;
    (void)x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 3000,
                                                x::runtime::main_thread::JobPrio::High);
}

void MaybeCastSpiritJavelin(DWORD now) {
    if (!x::features::ports::world::IsPlayReady()) return;
    if (!gSkillLvAt || now - gSkillLvAt >= kLevelCacheMs) {
        gSkillLv = x::features::ports::skill::GetSkillLevel(kSkillSpiritJavelin);
        gSkillLvAt = now ? now : 1;
    }
    if (gSkillLv <= 0) {
        if (!gLastNoSkillLog || now - gLastNoSkillLog >= kNoSkillLogMs) {
            gLastNoSkillLog = now ? now : 1;
            BinLog("skill %d lv=0（未学无形镖/無限飛鏢）；仅客户端冻数量，服端仍扣",
                   kSkillSpiritJavelin);
        }
        gLastRemain = 0.f;
        return;
    }
    float remain = 0.f;
    const bool active =
        x::features::ports::skill::IsSkillActive(kSkillSpiritJavelin, &remain);
    gLastRemain = remain;
    if (active && remain > kRenewRemainSec) return;
    if (gLastCastTry && now - gLastCastTry < kCastGapMs) return;
    gLastCastTry = now ? now : 1;
    bool notReady = false;
    char reason[48]{};
    const bool ok = x::features::ports::skill::CastSkill(
        kSkillSpiritJavelin, &notReady, reason, sizeof(reason),
        /*noPrepareFallback=*/true);
    if (ok)
        gCastOk.fetch_add(1, std::memory_order_relaxed);
    else
        gCastFail.fetch_add(1, std::memory_order_relaxed);
    BinLog("cast %d ok=%d nr=%d lv=%d remain=%.1f reason=%s", kSkillSpiritJavelin, ok ? 1 : 0,
           notReady ? 1 : 0, gSkillLv, remain, reason[0] ? reason : "-");
}

DWORD WINAPI Worker(LPVOID) {
    BinLog("worker start");
    DWORD lastHeart = 0;
    while (!gStop.load(std::memory_order_acquire)) {
        const bool want = gWant.load(std::memory_order_acquire);
        const bool need =
            gHooksWanted.load(std::memory_order_acquire) &&
            ((!gSet.active && !gSetRefuse.load(std::memory_order_relaxed)) ||
             (!gGet.active && !gGetRefuse.load(std::memory_order_relaxed)));
        if (need) RequestApply();
        const DWORD now = GetTickCount();
        if (want) MaybeCastSpiritJavelin(now);
        if (want && (!lastHeart || now - lastHeart >= kHeartMs)) {
            lastHeart = now ? now : 1;
            BinLog("heart set=%d get=%d want=%d blockDec=%u fakeGet=%u passSet=%u "
                   "skillLv=%d remain=%.1f castOk=%u castFail=%u",
                   gSet.active ? 1 : 0, gGet.active ? 1 : 0, want ? 1 : 0,
                   gBlockDec.load(std::memory_order_relaxed),
                   gFakeGet.load(std::memory_order_relaxed),
                   gPassSet.load(std::memory_order_relaxed), gSkillLv, gLastRemain,
                   gCastOk.load(std::memory_order_relaxed),
                   gCastFail.load(std::memory_order_relaxed));
        }
        Sleep(want ? 1000 : 2000);
    }
    BinLog("worker exit");
    return 0;
}

}  // namespace

void Init() {
    if (!xcat::kInfiniteStarsUserEnabled) {
        gWant.store(false, std::memory_order_relaxed);
        x::runtime::LogI("InfStars", "user gate off — skipped (keep code)");
        return;
    }
    gStop.store(false, std::memory_order_release);
    gHooksWanted.store(false, std::memory_order_release);
    gSetRefuse.store(false, std::memory_order_relaxed);
    gGetRefuse.store(false, std::memory_order_relaxed);
    OpenBinLog();
    BinLog("INIT default off; skill %d SpiritJavelin; Set/Get RVA 0x%X/0x%X; "
           "itemId/10000==207 freeze",
           kSkillSpiritJavelin, (unsigned)kRvaSetItemNumber, (unsigned)kRvaGetItemNumber);
    ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "init");
}

void Shutdown() {
    StopWorker();
    gWant.store(false, std::memory_order_release);
    gHooksWanted.store(false, std::memory_order_release);
    if (gSet.active || gGet.active) {
        if (x::runtime::main_thread::IsInstalled() && !x::runtime::main_thread::IsOnPumpThread())
            x::runtime::main_thread::InvokeAndWait(&PumpApply, nullptr, 2000);
        else {
            RemoveAbs(gGet);
            gGetTramp = nullptr;
            RemoveAbs(gSet);
            gSetTramp = nullptr;
        }
    }
    if (gLog != INVALID_HANDLE_VALUE) {
        CloseHandle(gLog);
        gLog = INVALID_HANDLE_VALUE;
    }
}

void StartWorker() {
    if (!xcat::kInfiniteStarsUserEnabled) {
        gWant.store(false, std::memory_order_relaxed);
        ReportLamp(x::runtime::anchor_lamps::AnchorLampCode::Unknown, "disabled");
        return;
    }
    if (gWorker.load(std::memory_order_acquire)) return;
    gStop.store(false, std::memory_order_release);
    HANDLE th = CreateThread(nullptr, 0, &Worker, nullptr, 0, nullptr);
    if (!th) {
        BinLog("CreateThread failed");
        return;
    }
    HANDLE prev = nullptr;
    if (!gWorker.compare_exchange_strong(prev, th, std::memory_order_acq_rel)) CloseHandle(th);
}

void StopWorker() {
    gStop.store(true, std::memory_order_release);
    HANDLE th = gWorker.exchange(nullptr, std::memory_order_acq_rel);
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
}

void SetEnabled(bool on) {
    if (!xcat::kInfiniteStarsUserEnabled) on = false;
    if (on && !EnsurePatchEnv()) {
        gWant.store(false, std::memory_order_release);
        return;
    }
    const bool was = gWant.exchange(on, std::memory_order_acq_rel);
    if (was == on) return;
    BinLog("switch %d→%d set=%d get=%d", was ? 1 : 0, on ? 1 : 0, gSet.active ? 1 : 0,
           gGet.active ? 1 : 0);
    if (!on) return;
    gSetRefuse.store(false, std::memory_order_relaxed);
    gGetRefuse.store(false, std::memory_order_relaxed);
    gSkillLv = -1;
    gSkillLvAt = 0;
    gLastCastTry = 0;
    gLastNoSkillLog = 0;
    gHooksWanted.store(true, std::memory_order_release);
    RequestApply();
}

bool IsEnabled() { return gWant.load(std::memory_order_acquire); }

}  // namespace x::features::infinite_stars
