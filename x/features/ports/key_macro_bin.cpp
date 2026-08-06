// Classic TWMS — KeyMacroAnalyzer 只读 BIN（SendInput vs Raw 句柄）。
// 2026-08-06 BIN：Put 被 RawInputHandler 直接 call（非 MethodInfo）→ MI 换槽捕不到。
// 主路径：解析单例 + 帧末读 hunt/keyboards（数据面）；MI 钩仅作辅（可能仍 0 hit）。
// 关：XCAT_KEYMACRO_BIN=0。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "key_macro_bin.h"

#include "../../runtime/bin_dir.h"
#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_container.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../../runtime/mono_clock.h"

#include <Windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>

namespace x::features::ports::key_macro_bin {
namespace {

using x::runtime::NowMs;
using x::runtime::il2cpp::LooksLikeHeapPtr;
using x::runtime::il2cpp::ReadPtr;

// remount 2026-08-06（旧 a408fa41…）
constexpr char kHashKeyMacro[] =
    "c4c281c97ed3eeacc4f369342b5161c85baa641982be5d14792a9ad0f1b53b3";

constexpr uint32_t kRvaSetHunting = 0x3C2B570;
constexpr uint32_t kRvaHandleCheck = 0x3C30660;
constexpr uint32_t kRvaIsSameHandle = 0x3C31850;

constexpr size_t kOffAntiHandle = 0x30;
constexpr size_t kOffHuntHandle = 0x38;
constexpr size_t kOffCollectHandle = 0x40;
constexpr size_t kOffRuneHandle = 0x48;
constexpr size_t kOffKeyboards = 0x78;  // Dictionary<IntPtr, KeyMacroCheckObject>
constexpr size_t kOffInitFlag = 0x20;   // bool init

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
    void* invokerMethod;
    const void* methodDefinition;
};

using FnSetHandle = void(__fastcall*)(void* self, intptr_t handle, const void* methodInfo);
using FnVoidSelf = void(__fastcall*)(void* self, const void* methodInfo);
using FnBoolSelf = uint8_t(__fastcall*)(void* self, const void* methodInfo);

std::atomic<bool> gEnabled{false};
std::atomic<bool> gInited{false};
std::atomic<void*> gAnalyzer{nullptr};
void* gKlass = nullptr;

FnSetHandle gOrigSetHunt = nullptr;
FnVoidSelf gOrigHandleChk = nullptr;
FnBoolSelf gOrigIsSame = nullptr;
MethodInfoHead* gMiSetHunt = nullptr;
MethodInfoHead* gMiHandleChk = nullptr;
MethodInfoHead* gMiIsSame = nullptr;

FILE* gLog = nullptr;
CRITICAL_SECTION gLogCs{};
bool gLogCsInit = false;

std::atomic<uint32_t> gOsSendHits{0};
std::atomic<uint32_t> gHandleChkHits{0};
std::atomic<uint32_t> gIsSameFalse{0};
std::atomic<DWORD> gLastOsSendMs{0};

DWORD gLastSnapMs = 0;
int gLastOsL = -1;
int gLastOsR = -1;
intptr_t gLastHunt = 0;
int gLastKbCount = -1;

void EnsureLogCs() {
    if (gLogCsInit) return;
    InitializeCriticalSection(&gLogCs);
    gLogCsInit = true;
}

void OpenLog() {
    EnsureLogCs();
    EnterCriticalSection(&gLogCs);
    if (!gLog) {
        char path[MAX_PATH]{};
        snprintf(path, sizeof(path), "%slogs\\key_macro_bin.log", x::runtime::GetBinDir());
        gLog = fopen(path, "a");
        if (gLog) {
            fprintf(gLog,
                    "# key_macro_bin RO v2 — data-plane hunt/keyboards (+ MI aux)\n"
                    "# Put is direct-call from RawInputHandler → MI Put hook dead\n"
                    "# hand L/R ≥1s then F5 HoldWalk; compare hunt + kbKeys\n");
            fflush(gLog);
        }
    }
    LeaveCriticalSection(&gLogCs);
}

void LogLine(const char* fmt, ...) {
    if (!gEnabled.load(std::memory_order_relaxed)) return;
    OpenLog();
    EnsureLogCs();
    EnterCriticalSection(&gLogCs);
    if (gLog) {
        fprintf(gLog, "%lu ", (unsigned long)GetTickCount());
        va_list ap;
        va_start(ap, fmt);
        vfprintf(gLog, fmt, ap);
        va_end(ap);
        fputc('\n', gLog);
        fflush(gLog);
    }
    LeaveCriticalSection(&gLogCs);
}

bool EnvOn() {
    char buf[8]{};
    const DWORD n = GetEnvironmentVariableA("XCAT_KEYMACRO_BIN", buf, sizeof(buf));
    if (n == 0) return true;
    return !(buf[0] == '0' && buf[1] == '\0');
}

intptr_t RdHandle(void* self, size_t off) {
    if (!self) return 0;
    intptr_t v = 0;
    __try {
        v = *reinterpret_cast<intptr_t*>(reinterpret_cast<uint8_t*>(self) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = -1;
    }
    return v;
}

uint8_t RdU8(void* self, size_t off) {
    if (!self) return 0;
    uint8_t v = 0;
    __try {
        v = *reinterpret_cast<uint8_t*>(reinterpret_cast<uint8_t*>(self) + off);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        v = 0;
    }
    return v;
}

bool ObjIsKlass(void* obj, void* klass) {
    if (!LooksLikeHeapPtr(obj) || !klass) return false;
    void* k = nullptr;
    __try {
        k = *reinterpret_cast<void**>(obj);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return k == klass;
}

void* StaticsOf(void* klass) {
    if (!klass) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!e.classStaticData) return nullptr;
    void* sd = nullptr;
    __try {
        sd = e.classStaticData(klass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        sd = nullptr;
    }
    return LooksLikeHeapPtr(sd) ? sd : nullptr;
}

void* PickInstanceFromStatics(void* sd, void* klass) {
    if (!sd || !klass) return nullptr;
    for (size_t s = 0; s < 8; ++s) {
        void* cand = nullptr;
        __try {
            cand = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(sd) + s * sizeof(void*));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            cand = nullptr;
        }
        // Lazy<> / 直接实例
        if (LooksLikeHeapPtr(cand) && !ObjIsKlass(cand, klass)) {
            void* inner = ReadPtr(cand, 0x10);  // 常见 Lazy.value
            if (ObjIsKlass(inner, klass)) cand = inner;
            else {
                inner = ReadPtr(cand, 0x8);
                if (ObjIsKlass(inner, klass)) cand = inner;
            }
        }
        if (ObjIsKlass(cand, klass)) return cand;
    }
    return nullptr;
}

void* EnsureAnalyzer() {
    void* cached = gAnalyzer.load(std::memory_order_acquire);
    if (ObjIsKlass(cached, gKlass)) return cached;

    if (!x::runtime::il2cpp::Ensure()) return nullptr;
    const auto& e = x::runtime::il2cpp::Get();
    if (!gKlass) gKlass = x::runtime::il2cpp::FindClass("", kHashKeyMacro);
    if (!gKlass) return nullptr;

    if (e.runtimeClassInit) {
        __try {
            e.runtimeClassInit(gKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* parent = nullptr;
    if (e.classParent) {
        __try {
            parent = e.classParent(gKlass);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            parent = nullptr;
        }
    }
    if (parent && e.runtimeClassInit) {
        __try {
            e.runtimeClassInit(parent);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }

    void* inst = PickInstanceFromStatics(StaticsOf(parent), gKlass);
    if (!inst) inst = PickInstanceFromStatics(StaticsOf(gKlass), gKlass);
    if (inst) gAnalyzer.store(inst, std::memory_order_release);
    return inst;
}

// Dictionary<IntPtr,T*>：采最多 4 个存活 key（设备句柄）。
int ReadKeyboardHandles(void* self, intptr_t* out, int maxOut) {
    if (!self || !out || maxOut <= 0) return 0;
    x::runtime::il2cpp_container::Ensure();
    void* dict = ReadPtr(self, kOffKeyboards);
    if (!LooksLikeHeapPtr(dict)) return 0;
    x::runtime::il2cpp_container::RefineFromDictInstance(dict);

    const size_t offCount = x::runtime::il2cpp_container::OffDictCount();
    const size_t offEntries = x::runtime::il2cpp_container::OffDictEntries();
    const size_t offFree = x::runtime::il2cpp_container::OffDictFreeCount();
    const size_t stride = x::runtime::il2cpp_container::DictEntryStrideIntPtr();

    int count = 0;
    int freeCount = 0;
    void* entries = nullptr;
    __try {
        count = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(dict) + offCount);
        freeCount = *reinterpret_cast<int*>(reinterpret_cast<uint8_t*>(dict) + offFree);
        entries = *reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(dict) + offEntries);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    const int live = count - freeCount;
    if (live <= 0 || !LooksLikeHeapPtr(entries)) return 0;

    int n = 0;
    const int scan = (live > 64) ? 64 : live;
    for (int i = 0; i < scan && n < maxOut; ++i) {
        uint8_t* ent = x::runtime::il2cpp_container::DictEntryAt(entries, i, stride);
        if (!ent) continue;
        int hash = 0;
        intptr_t key = 0;
        __try {
            hash = *reinterpret_cast<int*>(ent + x::runtime::il2cpp_container::OffDictEntryHash());
            key = *reinterpret_cast<intptr_t*>(ent + x::runtime::il2cpp_container::OffDictEntryKey());
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            continue;
        }
        if (hash < 0) continue;  // free slot
        if (!key) continue;
        out[n++] = key;
    }
    return n;
}

void Snapshot(void* self, const char* tag) {
    if (!self) {
        LogLine("%s self=null", tag);
        return;
    }
    const intptr_t anti = RdHandle(self, kOffAntiHandle);
    const intptr_t hunt = RdHandle(self, kOffHuntHandle);
    const intptr_t coll = RdHandle(self, kOffCollectHandle);
    const intptr_t rune = RdHandle(self, kOffRuneHandle);
    const int init = RdU8(self, kOffInitFlag) ? 1 : 0;
    const SHORT l = GetAsyncKeyState(VK_LEFT);
    const SHORT r = GetAsyncKeyState(VK_RIGHT);
    intptr_t kb[4]{};
    const int kn = ReadKeyboardHandles(self, kb, 4);
    const DWORD osAge = NowMs() - gLastOsSendMs.load(std::memory_order_relaxed);
    LogLine("%s self=%p init=%d anti=%p hunt=%p coll=%p rune=%p kbN=%d kb0=%p kb1=%p "
            "osL=%d osR=%d osAge=%lu osSend=%u chk=%u sameF=%u",
            tag, self, init, (void*)anti, (void*)hunt, (void*)coll, (void*)rune, kn, (void*)kb[0],
            (void*)kb[1], (l & 0x8000) ? 1 : 0, (r & 0x8000) ? 1 : 0, (unsigned long)osAge,
            gOsSendHits.load(std::memory_order_relaxed),
            gHandleChkHits.load(std::memory_order_relaxed),
            gIsSameFalse.load(std::memory_order_relaxed));
}

bool PatchMethodInfo(MethodInfoHead* mi, void* hook, void** outOrig) {
    if (!mi || !hook || !outOrig) return false;
    void* orig = nullptr;
    __try {
        orig = mi->methodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    if (!orig || orig == hook) return false;
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

void __fastcall HookSetHunt(void* self, intptr_t handle, const void* methodInfo) {
    gAnalyzer.store(self, std::memory_order_release);
    LogLine("setHunt h=%p (was=%p)", (void*)handle, (void*)RdHandle(self, kOffHuntHandle));
    if (gOrigSetHunt) gOrigSetHunt(self, handle, methodInfo);
    Snapshot(self, "snap@setHunt");
}

void __fastcall HookHandleChk(void* self, const void* methodInfo) {
    gAnalyzer.store(self, std::memory_order_release);
    gHandleChkHits.fetch_add(1, std::memory_order_relaxed);
    Snapshot(self, "handleChk");
    if (gOrigHandleChk) gOrigHandleChk(self, methodInfo);
}

uint8_t __fastcall HookIsSame(void* self, const void* methodInfo) {
    gAnalyzer.store(self, std::memory_order_release);
    uint8_t ret = 1;
    if (gOrigIsSame) ret = gOrigIsSame(self, methodInfo);
    if (!ret) gIsSameFalse.fetch_add(1, std::memory_order_relaxed);
    Snapshot(self, ret ? "isSame=1" : "isSame=0");
    return ret;
}

void BinFrameTick(void* /*user*/) {
    if (!gEnabled.load(std::memory_order_acquire)) return;
    void* self = EnsureAnalyzer();
    const int osL = (GetAsyncKeyState(VK_LEFT) & 0x8000) ? 1 : 0;
    const int osR = (GetAsyncKeyState(VK_RIGHT) & 0x8000) ? 1 : 0;
    const DWORD now = NowMs();
    const bool keyEdge = (osL != gLastOsL) || (osR != gLastOsR);
    gLastOsL = osL;
    gLastOsR = osR;

    intptr_t hunt = 0;
    int kbN = 0;
    if (self) {
        hunt = RdHandle(self, kOffHuntHandle);
        intptr_t kb[4]{};
        kbN = ReadKeyboardHandles(self, kb, 4);
    }
    const bool huntEdge = (hunt != gLastHunt);
    const bool kbEdge = (kbN != gLastKbCount);
    gLastHunt = hunt;
    gLastKbCount = kbN;

    if (keyEdge) {
        const DWORD osAge = now - gLastOsSendMs.load(std::memory_order_relaxed);
        Snapshot(self, (osAge < 80) ? "key@osSend" : "key@hand?");
    } else if (huntEdge || kbEdge) {
        Snapshot(self, huntEdge ? "huntChg" : "kbChg");
    } else if (now - gLastSnapMs >= 1000) {
        gLastSnapMs = now;
        Snapshot(self, self ? "tick" : "tick_no_self");
    }
}

bool InstallAuxHooks() {
    if (!x::runtime::il2cpp::Ensure()) return false;
    if (!gKlass) gKlass = x::runtime::il2cpp::FindClass("", kHashKeyMacro);
    if (!gKlass) {
        LogLine("install fail: klass miss");
        return false;
    }
    auto resolve = [](void* k, uint32_t rva) -> MethodInfoHead* {
        return reinterpret_cast<MethodInfoHead*>(
            x::runtime::il2cpp_method::FindMethodByRva(k, rva, true));
    };
    gMiSetHunt = resolve(gKlass, kRvaSetHunting);
    gMiHandleChk = resolve(gKlass, kRvaHandleCheck);
    gMiIsSame = resolve(gKlass, kRvaIsSameHandle);
    int ok = 0;
    void* orig = nullptr;
    if (gMiSetHunt && PatchMethodInfo(gMiSetHunt, reinterpret_cast<void*>(&HookSetHunt), &orig)) {
        gOrigSetHunt = reinterpret_cast<FnSetHandle>(orig);
        ++ok;
    }
    orig = nullptr;
    if (gMiHandleChk &&
        PatchMethodInfo(gMiHandleChk, reinterpret_cast<void*>(&HookHandleChk), &orig)) {
        gOrigHandleChk = reinterpret_cast<FnVoidSelf>(orig);
        ++ok;
    }
    orig = nullptr;
    if (gMiIsSame && PatchMethodInfo(gMiIsSame, reinterpret_cast<void*>(&HookIsSame), &orig)) {
        gOrigIsSame = reinterpret_cast<FnBoolSelf>(orig);
        ++ok;
    }
    LogLine("install v2 klass=%p auxMI=%d/3 (Put skipped: direct-call) setHunt=%p chk=%p same=%p",
            gKlass, ok, (void*)gMiSetHunt, (void*)gMiHandleChk, (void*)gMiIsSame);
    return true;
}

void UninstallAuxHooks() {
    if (gMiSetHunt && gOrigSetHunt)
        RestoreMethodInfo(gMiSetHunt, reinterpret_cast<void*>(gOrigSetHunt));
    if (gMiHandleChk && gOrigHandleChk)
        RestoreMethodInfo(gMiHandleChk, reinterpret_cast<void*>(gOrigHandleChk));
    if (gMiIsSame && gOrigIsSame)
        RestoreMethodInfo(gMiIsSame, reinterpret_cast<void*>(gOrigIsSame));
    gOrigSetHunt = nullptr;
    gOrigHandleChk = nullptr;
    gOrigIsSame = nullptr;
    gMiSetHunt = gMiHandleChk = gMiIsSame = nullptr;
}

}  // namespace

bool Enabled() { return gEnabled.load(std::memory_order_acquire); }

void NoteOsSendInput(WORD vk, bool down, UINT sent) {
    if (!gEnabled.load(std::memory_order_acquire)) return;
    gOsSendHits.fetch_add(1, std::memory_order_relaxed);
    gLastOsSendMs.store(NowMs(), std::memory_order_release);
    LogLine("osSend vk=0x%02X %s sent=%u osL=%d osR=%d", (unsigned)vk, down ? "DN" : "UP",
            (unsigned)sent, (GetAsyncKeyState(VK_LEFT) & 0x8000) ? 1 : 0,
            (GetAsyncKeyState(VK_RIGHT) & 0x8000) ? 1 : 0);
}

void Init() {
    if (gInited.exchange(true)) return;
    if (!EnvOn()) {
        gEnabled.store(false);
        x::runtime::LogI("KeyMacroBin", "XCAT_KEYMACRO_BIN=0 — off");
        return;
    }
    gEnabled.store(true);
    OpenLog();
    LogLine("init enabled=1 mode=data-plane+auxMI");
    if (x::runtime::main_thread::Ensure()) {
        x::runtime::main_thread::SetAuxFrameTick(&BinFrameTick, nullptr);
        (void)InstallAuxHooks();
        (void)EnsureAnalyzer();
        Snapshot(gAnalyzer.load(std::memory_order_acquire), "boot");
    }
    x::runtime::LogI("KeyMacroBin", "ON v2 → key_macro_bin.log (hunt/keyboards poll)");
}

void Shutdown() {
    if (!gInited.exchange(false)) return;
    gEnabled.store(false);
    x::runtime::main_thread::SetAuxFrameTick(nullptr, nullptr);
    UninstallAuxHooks();
    EnsureLogCs();
    EnterCriticalSection(&gLogCs);
    if (gLog) {
        fclose(gLog);
        gLog = nullptr;
    }
    LeaveCriticalSection(&gLogCs);
}

}  // namespace x::features::ports::key_macro_bin
