// Classic TWMS — mob_pool_observe（怪物刷新感知增强）
//
// P0b：MI 观察 OnPacketMobEnterField / LeaveField → RequestImmediateScan。
// 不改 mob_scan 周期逻辑；关开关即 Restore MI，行为回退为纯轮询。
// 禁止 INLINE HOOK（E9 / .text）。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "mob_pool_observe.h"

#include "../mob_scan/mob_scan.h"
#include "../../runtime/dbg_log_file.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

namespace x::features::mob_pool_observe {
namespace {

// SSOT class hash：与 ports/mob_pool_port.cpp 同代（dump.cs.restored MobPool）。
constexpr char kMobPoolClass[] =
    "de0bfa635af7881c2355ef947532ea53b0813b6bc6f2091a48411ea04756f54";

// dump.cs.restored：OnMouseMove 后首两个 InPacket = Enter / Leave（P0a 位序，跨 dump 稳定）。
// Il2CppDumper 写的是 CFF 体内 RVA；运行时 MethodInfo.methodPointer 与
// codeGenModule->methodPointers[] 都是 IDA 函数头（BIN 05:28：0xF973D0 / 0xF979F0）。
// 包分发走 methodPointers 表拷贝，只换 MI 会 install ok 但 obs 零命中。
constexpr uint32_t kRvaEnterDump = 0xF951D0;
constexpr uint32_t kRvaLeaveDump = 0xF957F0;
constexpr uint32_t kRvaEnterFn = 0xF973D0;
constexpr uint32_t kRvaLeaveFn = 0xF979F0;
constexpr uint32_t kRvaMouseFn = 0xF96F90;    // OnMouseMove 函数头（dump 体内 0xF94D90）
constexpr uint32_t kRvaMouseDump = 0xF94D90;
constexpr uint32_t kRvaTableEnter = 0x690F680;  // .data methodPointers Enter；Leave=+8
constexpr char kHashEnterField[] =
    "f4d168496bc77fc6eeddbbb2163f03094e217722180530102905993816e4985";
constexpr char kHashLeaveField[] =
    "f32b1c5ad6c0ea331c9d6feb87967122785b27710a24d9ab9e9209111dd0edf";

constexpr DWORD kInstallRetryMs = 3000;
constexpr DWORD kWorkerIdleMs = 500;
constexpr DWORD kLogThrottleMs = 800;  // enter/leave 边沿日志节流

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invoker;
    const void* nameOrHandle;
};

// void OnPacket*(InPacket*) — thiscall + MethodInfo*
using FnPacket = void (*)(void* self, void* pkt, void* methodInfo);

std::atomic<bool> gDesired{false};
std::atomic<bool> gInstalled{false};
std::atomic<bool> gStop{false};
std::atomic<HANDLE> gWorker{nullptr};
std::atomic<uint32_t> gEnterHits{0};
std::atomic<uint32_t> gLeaveHits{0};
std::atomic<DWORD> gLastEnterLog{0};
std::atomic<DWORD> gLastLeaveLog{0};

MethodInfoHead* gMiEnter = nullptr;
MethodInfoHead* gMiLeave = nullptr;
FnPacket gOrigEnter = nullptr;
FnPacket gOrigLeave = nullptr;
void** gTableEnter[4]{};
void** gTableLeave[4]{};
int gTableN = 0;
void* gKlass = nullptr;
DWORD gLastInstallTry = 0;
HANDLE gLog = INVALID_HANDLE_VALUE;

void ReturnLeakedMetadataLock(const char* where) {
    x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(where);
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
    gLog = x::runtime::OpenRotatingDbgLog(dir, L"mobpool_obs.log");
}

void LogLine(const char* fmt, ...) {
    OpenLog();
    if (gLog == INVALID_HANDLE_VALUE) return;
    char body[800];
    va_list ap;
    va_start(ap, fmt);
    int bn = vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);
    if (bn < 0) return;
    if (bn >= (int)sizeof(body)) bn = (int)sizeof(body) - 1;
    body[bn] = '\0';

    char buf[900];
    SYSTEMTIME st{};
    GetLocalTime(&st);
    int n = snprintf(buf, sizeof(buf), "%02u:%02u:%02u.%03u %s\n", st.wHour, st.wMinute,
                     st.wSecond, st.wMilliseconds, body);
    if (n < 0) return;
    if (n >= (int)sizeof(buf)) n = (int)sizeof(buf) - 1;
    DWORD w = 0;
    WriteFile(gLog, buf, (DWORD)n, &w, nullptr);
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = mi->methodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig) return false;
    if (orig == hook) return false;  // already ours — caller must keep saved orig
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        mi->methodPointer = hook;
        if (mi->virtualMethodPointer == orig) mi->virtualMethodPointer = hook;
        *outOrig = orig;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
    return ok;
}

void RestoreMethodInfo(MethodInfoHead* mi, void* orig) {
    if (!mi || !orig) return;
    DWORD old = 0;
    if (!VirtualProtect(mi, sizeof(MethodInfoHead), PAGE_READWRITE, &old)) return;
    __try {
        void* cur = mi->methodPointer;
        mi->methodPointer = orig;
        if (mi->virtualMethodPointer == cur) mi->virtualMethodPointer = orig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    VirtualProtect(mi, sizeof(MethodInfoHead), old, &old);
}

bool HookStillOurs(MethodInfoHead* mi, void* hook) {
    if (!mi || !hook) return false;
    __try {
        return mi->methodPointer == hook;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void WakeScan(bool isEnter) {
    mob_scan::RequestImmediateScan();
    const DWORD now = GetTickCount();
    if (isEnter) {
        const uint32_t n = gEnterHits.fetch_add(1, std::memory_order_relaxed) + 1;
        const DWORD prev = gLastEnterLog.load(std::memory_order_relaxed);
        if (!prev || now - prev >= kLogThrottleMs) {
            gLastEnterLog.store(now, std::memory_order_relaxed);
            LogLine("obs enter via=MI wake hits=%u", n);
        }
    } else {
        const uint32_t n = gLeaveHits.fetch_add(1, std::memory_order_relaxed) + 1;
        const DWORD prev = gLastLeaveLog.load(std::memory_order_relaxed);
        if (!prev || now - prev >= kLogThrottleMs) {
            gLastLeaveLog.store(now, std::memory_order_relaxed);
            LogLine("obs leave via=MI wake hits=%u", n);
        }
    }
}

void Hook_Enter(void* self, void* pkt, void* methodInfo) {
    FnPacket orig = gOrigEnter;
    if (orig) {
        __try {
            orig(self, pkt, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (gDesired.load(std::memory_order_relaxed)) WakeScan(true);
}

void Hook_Leave(void* self, void* pkt, void* methodInfo) {
    FnPacket orig = gOrigLeave;
    if (orig) {
        __try {
            orig(self, pkt, methodInfo);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    if (gDesired.load(std::memory_order_relaxed)) WakeScan(false);
}

uint32_t PtrToGaRva(void* p) {
    const uintptr_t base = x::runtime::il2cpp::GaBase();
    if (!base || !p) return 0;
    const auto a = reinterpret_cast<uintptr_t>(p);
    if (a < base) return 0;
    const uint64_t d = static_cast<uint64_t>(a - base);
    return d > 0x7FFFFFFFull ? 0u : static_cast<uint32_t>(d);
}

uint32_t MiLiveRva(MethodInfoHead* mi) {
    if (!mi) return 0;
    void* mp = nullptr;
    void* vp = nullptr;
    __try {
        mp = mi->methodPointer;
        vp = mi->virtualMethodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    const uint32_t r = PtrToGaRva(mp);
    return r ? r : PtrToGaRva(vp);
}

const char* MiName(MethodInfoHead* mi) {
    const auto& e = x::runtime::il2cpp::Get();
    if (!mi || !e.methodGetName) return "";
    const char* nm = nullptr;
    __try {
        nm = e.methodGetName(mi);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "";
    }
    return nm ? nm : "";
}

int CollectMethods(void* klass, MethodInfoHead** bag, int cap) {
    const auto& e = x::runtime::il2cpp::Get();
    if (!klass || !e.classGetMethods || cap <= 0) return 0;
    int n = 0;
    void* iter = nullptr;
    __try {
        for (;;) {
            auto* mi = reinterpret_cast<MethodInfoHead*>(e.classGetMethods(klass, &iter));
            if (!mi) break;
            if (n < cap) bag[n++] = mi;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLeakedMetadataLock("mobpool_obs/classGetMethods");
        return 0;
    }
    return n;
}

bool NameIs(const char* nm, const char* want) {
    return nm && want && nm[0] && want[0] && std::strcmp(nm, want) == 0;
}

bool PatchQword(void** slot, void* neu) {
    if (!slot || !neu) return false;
    DWORD old = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_EXECUTE_READWRITE, &old)) return false;
    bool ok = false;
    __try {
        *slot = neu;
        ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
    }
    VirtualProtect(slot, sizeof(void*), old, &old);
    return ok;
}

bool TryRecordPair(void** slotE, void** slotL, void* origE, void* origL, void* hookE, void* hookL) {
    if (!slotE || !slotL || gTableN >= 4) return false;
    void* gotE = nullptr;
    void* gotL = nullptr;
    __try {
        gotE = *slotE;
        gotL = *slotL;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (gotE != origE || gotL != origL) return false;
    if (!PatchQword(slotE, hookE) || !PatchQword(slotL, hookL)) {
        LogLine("table protect fail slot=0x%X", PtrToGaRva(slotE));
        return false;
    }
    gTableEnter[gTableN] = slotE;
    gTableLeave[gTableN] = slotL;
    ++gTableN;
    return true;
}

// il2cpp methodPointers[] 与 MI 各持一份函数头。包分发走表，必须两处都换。
// BIN 05:40：origM 为空时函数开头直接 return 0，表根本没扫。
int PatchMethodPointerTable(void* origM, void* origE, void* origL, void* hookE, void* hookL) {
    gTableN = 0;
    if (!origE || !origL || !hookE || !hookL) return 0;
    const uintptr_t base = x::runtime::il2cpp::GaBase();
    if (base && kRvaTableEnter) {
        auto** slotE = reinterpret_cast<void**>(base + kRvaTableEnter);
        auto** slotL = slotE + 1;
        if (TryRecordPair(slotE, slotL, origE, origL, hookE, hookL)) {
            LogLine("table hit via=rva slot=0x%X", kRvaTableEnter);
            return gTableN;
        }
    }
    HMODULE ga = x::runtime::il2cpp::GameAssembly();
    if (!ga) return 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(ga);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return 0;
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(ga) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return 0;
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
        const DWORD ch = sec[i].Characteristics;
        if (ch & IMAGE_SCN_MEM_EXECUTE) continue;
        auto* begin = reinterpret_cast<uint8_t*>(ga) + sec[i].VirtualAddress;
        const size_t sz = sec[i].Misc.VirtualSize;
        if (sz < 16) continue;
        uint8_t* end = begin + sz;
        uint8_t* cur = begin;
        while (cur + 16 <= end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(cur, &mbi, sizeof(mbi)) != sizeof(mbi)) break;
            auto* rBegin = reinterpret_cast<uint8_t*>(mbi.BaseAddress);
            auto* rEnd = rBegin + mbi.RegionSize;
            if (rEnd > end) rEnd = end;
            const bool okProt =
                (mbi.State == MEM_COMMIT) &&
                !(mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD));
            if (!okProt) {
                cur = rEnd;
                continue;
            }
            if (cur < rBegin) cur = rBegin;
            for (; cur + 24 <= rEnd; cur += 8) {
                auto** p = reinterpret_cast<void**>(cur);
                bool match3 = false;
                bool match2 = false;
                __try {
                    if (origM) match3 = (p[0] == origM && p[1] == origE && p[2] == origL);
                    match2 = (p[0] == origE && p[1] == origL);
                } __except (EXCEPTION_EXECUTE_HANDLER) {
                    match3 = false;
                    match2 = false;
                }
                if (match3) {
                    if (TryRecordPair(p + 1, p + 2, origE, origL, hookE, hookL) && gTableN >= 4)
                        return gTableN;
                } else if (!origM && match2) {
                    if (TryRecordPair(p, p + 1, origE, origL, hookE, hookL) && gTableN >= 4)
                        return gTableN;
                }
            }
            if (cur < rEnd) cur = rEnd;
        }
    }
    return gTableN;
}

void RestoreMethodPointerTable() {
    for (int i = 0; i < gTableN; ++i) {
        if (gTableEnter[i] && gOrigEnter) {
            PatchQword(gTableEnter[i], reinterpret_cast<void*>(gOrigEnter));
        }
        if (gTableLeave[i] && gOrigLeave) {
            PatchQword(gTableLeave[i], reinterpret_cast<void*>(gOrigLeave));
        }
        gTableEnter[i] = nullptr;
        gTableLeave[i] = nullptr;
    }
    gTableN = 0;
}

bool ResolveEnterLeave(void* klass, MethodInfoHead** outE, MethodInfoHead** outL, const char** viaE,
                       const char** viaL, uint32_t* liveE, uint32_t* liveL) {
    if (viaE) *viaE = "MISS";
    if (viaL) *viaL = "MISS";
    if (liveE) *liveE = 0;
    if (liveL) *liveL = 0;
    if (outE) *outE = nullptr;
    if (outL) *outL = nullptr;
    MethodInfoHead* bag[256]{};
    const int n = CollectMethods(klass, bag, 256);
    if (n <= 0) return false;

    int mouse = -1;
    MethodInfoHead* byHashE = nullptr;
    MethodInfoHead* byHashL = nullptr;
    MethodInfoHead* byDumpE = nullptr;
    MethodInfoHead* byDumpL = nullptr;
    for (int i = 0; i < n; ++i) {
        const char* nm = MiName(bag[i]);
        const uint32_t rva = MiLiveRva(bag[i]);
        if (mouse < 0 && (NameIs(nm, "OnMouseMove") || rva == kRvaMouseFn || rva == kRvaMouseDump))
            mouse = i;
        if (NameIs(nm, kHashEnterField) || NameIs(nm, "OnPacketMobEnterField")) byHashE = bag[i];
        if (NameIs(nm, kHashLeaveField) || NameIs(nm, "OnPacketMobLeaveField")) byHashL = bag[i];
        if (rva == kRvaEnterDump || rva == kRvaEnterFn) byDumpE = bag[i];
        if (rva == kRvaLeaveDump || rva == kRvaLeaveFn) byDumpL = bag[i];
    }

    MethodInfoHead* miE = nullptr;
    MethodInfoHead* miL = nullptr;
    const char* ve = "MISS";
    const char* vl = "MISS";
    if (mouse >= 0 && mouse + 2 < n) {
        miE = bag[mouse + 1];
        miL = bag[mouse + 2];
        ve = "slot";
        vl = "slot";
    }
    if (!miE && byHashE) {
        miE = byHashE;
        ve = "name";
    }
    if (!miL && byHashL) {
        miL = byHashL;
        vl = "name";
    }
    if (!miE && byDumpE) {
        miE = byDumpE;
        ve = "rva";
    }
    if (!miL && byDumpL) {
        miL = byDumpL;
        vl = "rva";
    }
    if (!miE || !miL || miE == miL) return false;
    if (viaE) *viaE = ve;
    if (viaL) *viaL = vl;
    if (liveE) *liveE = MiLiveRva(miE);
    if (liveL) *liveL = MiLiveRva(miL);
    if (outE) *outE = miE;
    if (outL) *outL = miL;
    return true;
}

void LogMissScan(void* klass) {
    static bool sOnce = false;
    if (sOnce || !klass) return;
    sOnce = true;
    MethodInfoHead* bag[256]{};
    const int n = CollectMethods(klass, bag, 256);
    int mouse = -1;
    for (int i = 0; i < n; ++i) {
        if (NameIs(MiName(bag[i]), "OnMouseMove") || MiLiveRva(bag[i]) == kRvaMouseFn ||
            MiLiveRva(bag[i]) == kRvaMouseDump) {
            mouse = i;
            break;
        }
    }
    LogLine("miss scan methods=%d OnMouseMove idx=%d", n, mouse);
    const int lo = (mouse >= 0) ? mouse : 0;
    const int hi = (mouse >= 0) ? (mouse + 4 < n ? mouse + 4 : n) : (n < 8 ? n : 8);
    for (int i = lo; i < hi; ++i) {
        LogLine("miss scan [%d] rva=0x%X name=%.80s", i, MiLiveRva(bag[i]), MiName(bag[i]));
    }
}

void Uninstall() {
    RestoreMethodPointerTable();
    if (gMiEnter && gOrigEnter) {
        RestoreMethodInfo(gMiEnter, reinterpret_cast<void*>(gOrigEnter));
    }
    if (gMiLeave && gOrigLeave) {
        RestoreMethodInfo(gMiLeave, reinterpret_cast<void*>(gOrigLeave));
    }
    gMiEnter = nullptr;
    gMiLeave = nullptr;
    gOrigEnter = nullptr;
    gOrigLeave = nullptr;
    gInstalled.store(false, std::memory_order_release);
}

bool TryInstallOnPump() {
    if (!x::runtime::il2cpp::Ensure()) return false;
    if (!gKlass) gKlass = x::runtime::il2cpp::FindClass("", kMobPoolClass);
    if (!gKlass) {
        LogLine("install miss: MobPool klass");
        return false;
    }
    (void)x::runtime::il2cpp::RuntimeClassInit(gKlass);

    const char* viaE = "MISS";
    const char* viaL = "MISS";
    uint32_t liveE = 0;
    uint32_t liveL = 0;
    MethodInfoHead* miE = nullptr;
    MethodInfoHead* miL = nullptr;
    if (!ResolveEnterLeave(gKlass, &miE, &miL, &viaE, &viaL, &liveE, &liveL) || !miE || !miL) {
        LogLine("install miss: mi enter=%p(%s orig=0x%X) leave=%p(%s orig=0x%X)", miE, viaE, liveE,
                miL, viaL, liveL);
        LogMissScan(gKlass);
        return false;
    }

    MethodInfoHead* bag[256]{};
    const int n = CollectMethods(gKlass, bag, 256);
    void* origM = nullptr;
    for (int i = 0; i < n; ++i) {
        const uint32_t rva = MiLiveRva(bag[i]);
        if (NameIs(MiName(bag[i]), "OnMouseMove") || rva == kRvaMouseFn || rva == kRvaMouseDump) {
            origM = bag[i]->methodPointer;
            break;
        }
    }
    if (!origM) origM = x::runtime::il2cpp::AtRva<void*>(kRvaMouseFn);

    void* origE = nullptr;
    void* origL = nullptr;
    if (!PatchMethodInfo(miE, reinterpret_cast<void*>(&Hook_Enter), &origE) || !origE) {
        LogLine("install fail: patch enter");
        return false;
    }
    if (!PatchMethodInfo(miL, reinterpret_cast<void*>(&Hook_Leave), &origL) || !origL) {
        RestoreMethodInfo(miE, origE);
        LogLine("install fail: patch leave (enter restored)");
        return false;
    }

    gMiEnter = miE;
    gMiLeave = miL;
    gOrigEnter = reinterpret_cast<FnPacket>(origE);
    gOrigLeave = reinterpret_cast<FnPacket>(origL);
    const int tableHits =
        PatchMethodPointerTable(origM, origE, origL, reinterpret_cast<void*>(&Hook_Enter),
                                reinterpret_cast<void*>(&Hook_Leave));
    gInstalled.store(true, std::memory_order_release);
    LogLine("install ok enter via=%s orig=0x%X name=%.24s leave via=%s orig=0x%X name=%.24s table=%d",
            viaE, liveE ? liveE : PtrToGaRva(origE), MiName(miE), viaL,
            liveL ? liveL : PtrToGaRva(origL), MiName(miL), tableHits);
    x::runtime::LogI("MobPoolObs", "MI+table installed via=%s/%s orig=0x%X/0x%X table=%d", viaE, viaL,
                     liveE, liveL, tableHits);
    if (tableHits <= 0) {
        LogLine("install warn: methodPointers table not found — packet path may miss");
    }
    return true;
}

bool TryInstall() {
    if (x::runtime::main_thread::IsOnPumpThread()) return TryInstallOnPump();
    bool ok = false;
    auto job = [](void* p) {
        *static_cast<bool*>(p) = TryInstallOnPump();
    };
    if (!x::runtime::main_thread::InvokeAndWait(job, &ok, 2500,
                                                x::runtime::main_thread::JobPrio::High)) {
        LogLine("install miss: pump timeout");
        return false;
    }
    return ok;
}

bool MaintainInstalled() {
    if (!gInstalled.load(std::memory_order_acquire)) return false;
    const bool eOk =
        HookStillOurs(gMiEnter, reinterpret_cast<void*>(&Hook_Enter)) && gOrigEnter;
    const bool lOk =
        HookStillOurs(gMiLeave, reinterpret_cast<void*>(&Hook_Leave)) && gOrigLeave;
    bool tOk = true;
    for (int i = 0; i < gTableN; ++i) {
        void* pe = nullptr;
        void* pl = nullptr;
        __try {
            pe = gTableEnter[i] ? *gTableEnter[i] : nullptr;
            pl = gTableLeave[i] ? *gTableLeave[i] : nullptr;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            tOk = false;
            break;
        }
        if (pe != reinterpret_cast<void*>(&Hook_Enter) ||
            pl != reinterpret_cast<void*>(&Hook_Leave)) {
            tOk = false;
            break;
        }
    }
    if (eOk && lOk && tOk) return true;
    LogLine("maintain: hook lost — uninstall");
    Uninstall();
    return false;
}

DWORD WINAPI Worker(LPVOID) {
    OpenLog();
    LogLine("worker start");
    while (!gStop.load(std::memory_order_acquire)) {
        const bool want = gDesired.load(std::memory_order_relaxed);
        if (!want) {
            if (gInstalled.load(std::memory_order_acquire)) {
                Uninstall();
                LogLine("disabled — MI restored");
            }
            Sleep(kWorkerIdleMs);
            continue;
        }
        if (!MaintainInstalled()) {
            const DWORD now = GetTickCount();
            if (!gLastInstallTry || now - gLastInstallTry >= kInstallRetryMs) {
                gLastInstallTry = now;
                (void)TryInstall();
            }
        }
        Sleep(kWorkerIdleMs);
    }
    Uninstall();
    LogLine("worker stop");
    return 0;
}

}  // namespace

void Init() {
    gDesired.store(false);
    OpenLog();
    LogLine("Init — default off; MI Enter fn@0x%X dump@0x%X Leave fn@0x%X dump@0x%X",
            kRvaEnterFn, kRvaEnterDump, kRvaLeaveFn, kRvaLeaveDump);
    x::runtime::LogI("MobPoolObs", "init (default off)");
}

void Shutdown() {
    gDesired.store(false);
    StopWorker();
    Uninstall();
}

void StartWorker() {
    if (gWorker.load()) return;
    gStop.store(false);
    HANDLE th = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    if (th) {
        gWorker.store(th);
    } else {
        LogLine("StartWorker CreateThread failed");
    }
}

void StopWorker() {
    gStop.store(true);
    HANDLE th = gWorker.exchange(nullptr);
    if (th) {
        WaitForSingleObject(th, 3000);
        CloseHandle(th);
    }
    Uninstall();
}

void SetEnabled(bool on) {
    const bool prev = gDesired.exchange(on);
    if (prev == on) return;
    LogLine("SetEnabled %d", on ? 1 : 0);
    x::runtime::LogI("MobPoolObs", "SetEnabled %d", on ? 1 : 0);
    if (!on) Uninstall();
}

bool IsEnabled() { return gDesired.load(std::memory_order_relaxed); }

bool IsInstalled() {
    return gInstalled.load(std::memory_order_acquire) && gOrigEnter && gOrigLeave;
}

}  // namespace x::features::mob_pool_observe
