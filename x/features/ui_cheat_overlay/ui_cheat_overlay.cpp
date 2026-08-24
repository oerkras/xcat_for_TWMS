// TWMS Classic — 实验：强制打开 UICheat IMGUI GM overlay。
//
// dump：类 hash c1f54c54… TypeDef 163，[UIPrefab("UICheat")]。
// OnGUI RVA 0xD25E60（含 LiveValue 525 等 GM 控件）。不直调 OnGUI：
// MonoBehaviour 挂在 active GO 上后由 Unity 每帧调用。
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include "ui_cheat_overlay.h"

#include <Windows.h>

#include "../../runtime/il2cpp_bind.h"
#include "../../runtime/il2cpp_metadata_lock.h"
#include "../../runtime/il2cpp_method.h"
#include "../../runtime/log.h"
#include "../../runtime/main_thread_pump.h"
#include "../ports/world_port.h"
#include "ui_cheat_zh.h"

#include <atomic>
#include <climits>
#include <cstdint>
#include <cstring>

namespace x::features::ui_cheat_overlay {

constexpr char kHashUiCheat[] =
    "c1f54c5425c636db05a846e2ca87d01172b37116cab2615cc5107ece3fb47f9";
constexpr char kHashIsInstantiated[] =
    "ff84f6924f6e96917e22c3a0baaf9cb245962212a2209398e27b160fee02fe8";
constexpr char kHashCreateInstance[] =
    "bdfee1f539b33969bdae9b3ff8b6141457975fa821d42b4c00772faed0cde7c";
constexpr char kHashGetInstance[] =
    "f6ff4a351af5f3989e757ef0e1fa4dbb40c3cb2b0a6d1f1257fd3e8f1d691a6";
constexpr char kHashOpen[] =
    "eb98051de038f22f6c41b64f23af11c80ebf4738587f1b92021581af57c4baf";

// runtime dump.cs（2026-08 活进程）；hash/plain 优先，RVA 仅 fallback。
constexpr uint32_t kRvaIsInstantiated = 0xD3B4C0;
constexpr uint32_t kRvaCreateInstance = 0xD3B870;
constexpr uint32_t kRvaGetInstance = 0xD3BAA0;
constexpr uint32_t kRvaOpen = 0x7E2F50;  // UIWindow.Open（父类 slot 28）
constexpr uint32_t kRvaGoCtorName = 0x4E98FE0;
constexpr uint32_t kRvaAddComponent = 0x4E97700;
constexpr uint32_t kRvaDontDestroy = 0x4EA11A0;
constexpr uint32_t kRvaGoSetActive = 0x4E97EE0;
constexpr uint32_t kRvaCompGetGo = 0x4E92710;
// GUIContent.Temp(string)：Button/Label/Box/Toggle 都是直 call 这个 RVA，
// MethodInfo->methodPointer 改写进不去（BIN 已证 hits=5/5 但 0 次 Translate）。
constexpr uint32_t kRvaGuiContentTemp = 0x4F0B410;

constexpr DWORD kPumpTimeoutMs = 8000;

namespace {

namespace il2 = x::runtime::il2cpp;
namespace world = x::features::ports::world;
using x::runtime::il2cpp_method::MethodShape;
using x::runtime::il2cpp_method::TypeKind;
using x::runtime::il2cpp_method::FindMethodResolved;
using x::runtime::il2cpp_method::PathName;

struct MethodInfoHead {
    void* methodPointer;
    void* virtualMethodPointer;
};

std::atomic<bool> gBusy{false};
std::atomic<bool> gStop{false};
std::atomic<bool> gJobDone{true};
std::atomic<bool> gZhInstalled{false};

struct ZhCacheEnt {
    void* src = nullptr;
    uint64_t hash = 0;
    void* dst = nullptr;
};

constexpr int kZhCacheCap = 1024;
ZhCacheEnt gZhCache[kZhCacheCap]{};
int gZhCacheN = 0;
int gZhMissLogN = 0;

uint64_t HashUtf8(const char* s) {
    uint64_t h = 14695981039346656037ull;
    if (!s) return 0;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(s); *p; ++p) {
        h ^= *p;
        h *= 1099511628211ull;
    }
    return h;
}

void* LookupZh(void* src, uint64_t hash) {
    for (int i = 0; i < gZhCacheN; ++i) {
        if (gZhCache[i].src == src && gZhCache[i].dst) return gZhCache[i].dst;
    }
    if (hash) {
        for (int i = 0; i < gZhCacheN; ++i) {
            if (gZhCache[i].hash == hash && gZhCache[i].dst) return gZhCache[i].dst;
        }
    }
    return nullptr;
}

void StoreZh(void* src, uint64_t hash, void* dst) {
    if (gZhCacheN >= kZhCacheCap || !dst) return;
    gZhCache[gZhCacheN].src = src;
    gZhCache[gZhCacheN].hash = hash;
    gZhCache[gZhCacheN].dst = dst;
    ++gZhCacheN;
}

void ReturnLock(const char* where) {
    x::runtime::il2cpp_metadata_lock::ReleaseIfOwnedByCurrentThread(where);
}

// GUIContent.Temp 序言（runtime dump）：push rsi; push rdi; sub rsp,28h; mov rsi,rcx
// 共 9 字节且无 RIP-relative。后面紧跟 cmp [rip+disp],0，不能 steal 12 做 abs-jmp。
constexpr uint8_t kTempSig[] = {0x56, 0x57, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0xCE};
constexpr size_t kTempSteal = sizeof(kTempSig);

struct AbsHookState {
    void* target = nullptr;
    void* trampoline = nullptr;
    void* nearStub = nullptr;
    uint8_t saved[32]{};
    size_t stolen = 0;
    bool active = false;
};

AbsHookState gTempAbs{};
using FnTemp = void* (*)(void* text, void* a2);
FnTemp gTempTramp = nullptr;
std::atomic<uint32_t> gTempHits{0};
std::atomic<DWORD> gTempPulseMs{0};

void WriteAbsJmp(void* at, void* to) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    p[0] = 0x48;
    p[1] = 0xB8;
    *reinterpret_cast<uint64_t*>(p + 2) = reinterpret_cast<uint64_t>(to);
    p[10] = 0xFF;
    p[11] = 0xE0;
}

bool WriteRelJmp5(void* at, void* to, size_t steal) {
    auto* p = reinterpret_cast<uint8_t*>(at);
    const intptr_t rel =
        reinterpret_cast<uint8_t*>(to) - (reinterpret_cast<uint8_t*>(at) + 5);
    if (rel < static_cast<intptr_t>(INT32_MIN) || rel > static_cast<intptr_t>(INT32_MAX)) {
        return false;
    }
    p[0] = 0xE9;
    const auto r32 = static_cast<int32_t>(rel);
    std::memcpy(p + 1, &r32, 4);
    for (size_t i = 5; i < steal; ++i) p[i] = 0x90;
    return true;
}

void* AllocNear(void* target, size_t size) {
    if (!target || size == 0) return nullptr;
    constexpr uintptr_t kGran = 0x10000;
    constexpr uintptr_t kReach = 0x70000000ull;
    const uintptr_t t = reinterpret_cast<uintptr_t>(target) & ~(kGran - 1);
    MEMORY_BASIC_INFORMATION mbi{};
    for (int dir = 0; dir < 2; ++dir) {
        for (uintptr_t off = kGran; off < kReach; off += kGran) {
            uintptr_t p = 0;
            if (dir == 0) {
                p = t + off;
            } else {
                if (t < off) break;
                p = t - off;
            }
            if (p < 0x10000) continue;
            if (!VirtualQuery(reinterpret_cast<void*>(p), &mbi, sizeof(mbi))) continue;
            if (mbi.State != MEM_FREE) continue;
            void* r = VirtualAlloc(reinterpret_cast<void*>(p), size, MEM_COMMIT | MEM_RESERVE,
                                   PAGE_EXECUTE_READWRITE);
            if (!r) continue;
            const intptr_t rel =
                reinterpret_cast<uint8_t*>(r) - (reinterpret_cast<uint8_t*>(target) + 5);
            if (rel >= static_cast<intptr_t>(INT32_MIN) &&
                rel <= static_cast<intptr_t>(INT32_MAX)) {
                return r;
            }
            VirtualFree(r, 0, MEM_RELEASE);
        }
    }
    return nullptr;
}

bool BytesMatch(void* p, const uint8_t* sig, size_t n) {
    if (!p || !sig || n == 0) return false;
    __try {
        return std::memcmp(p, sig, n) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RemoveTempAbs() {
    if (!gTempAbs.active || !gTempAbs.target) return;
    DWORD old = 0;
    if (VirtualProtect(gTempAbs.target, gTempAbs.stolen, PAGE_EXECUTE_READWRITE, &old)) {
        std::memcpy(gTempAbs.target, gTempAbs.saved, gTempAbs.stolen);
        FlushInstructionCache(GetCurrentProcess(), gTempAbs.target, gTempAbs.stolen);
        VirtualProtect(gTempAbs.target, gTempAbs.stolen, old, &old);
    }
    if (gTempAbs.trampoline) VirtualFree(gTempAbs.trampoline, 0, MEM_RELEASE);
    if (gTempAbs.nearStub) VirtualFree(gTempAbs.nearStub, 0, MEM_RELEASE);
    gTempAbs.trampoline = nullptr;
    gTempAbs.nearStub = nullptr;
    gTempAbs.target = nullptr;
    gTempAbs.stolen = 0;
    gTempAbs.active = false;
    gTempTramp = nullptr;
}

bool InstallTempRel5(void* target, void* hook) {
    if (gTempAbs.active) return true;
    if (!target || !hook) return false;
    if (!BytesMatch(target, kTempSig, kTempSteal)) {
        uint8_t live[9]{};
        __try {
            std::memcpy(live, target, sizeof(live));
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
        x::runtime::LogW("UiCheatOverlay",
                         "zh Temp Rel5 refuse: sig mismatch @%p "
                         "live=%02X%02X%02X%02X%02X%02X%02X%02X%02X",
                         target, live[0], live[1], live[2], live[3], live[4], live[5], live[6],
                         live[7], live[8]);
        return false;
    }
    void* tramp =
        VirtualAlloc(nullptr, kTempSteal + 16, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!tramp) return false;
    void* nearStub = AllocNear(target, 16);
    if (!nearStub) {
        VirtualFree(tramp, 0, MEM_RELEASE);
        x::runtime::LogW("UiCheatOverlay", "zh Temp Rel5 refuse: AllocNear failed @%p", target);
        return false;
    }
    std::memcpy(gTempAbs.saved, target, kTempSteal);
    std::memcpy(tramp, target, kTempSteal);
    WriteAbsJmp(reinterpret_cast<uint8_t*>(tramp) + kTempSteal,
                reinterpret_cast<uint8_t*>(target) + kTempSteal);
    WriteAbsJmp(nearStub, hook);
    gTempTramp = reinterpret_cast<FnTemp>(tramp);
    DWORD old = 0;
    if (!VirtualProtect(target, kTempSteal, PAGE_EXECUTE_READWRITE, &old)) {
        VirtualFree(nearStub, 0, MEM_RELEASE);
        VirtualFree(tramp, 0, MEM_RELEASE);
        gTempTramp = nullptr;
        return false;
    }
    if (!WriteRelJmp5(target, nearStub, kTempSteal)) {
        std::memcpy(target, gTempAbs.saved, kTempSteal);
        FlushInstructionCache(GetCurrentProcess(), target, kTempSteal);
        VirtualProtect(target, kTempSteal, old, &old);
        VirtualFree(nearStub, 0, MEM_RELEASE);
        VirtualFree(tramp, 0, MEM_RELEASE);
        gTempTramp = nullptr;
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), target, kTempSteal);
    FlushInstructionCache(GetCurrentProcess(), nearStub, 16);
    VirtualProtect(target, kTempSteal, old, &old);
    gTempAbs.target = target;
    gTempAbs.trampoline = tramp;
    gTempAbs.nearStub = nearStub;
    gTempAbs.stolen = kTempSteal;
    gTempAbs.active = true;
    return true;
}

bool ReadCaptionUtf8(void* str, char* out, int outSz) {
    if (!str || !out || outSz <= 1) return false;
    out[0] = 0;
    __try {
        const int len = *reinterpret_cast<int32_t*>(reinterpret_cast<uint8_t*>(str) + 0x10);
        if (len <= 0 || len > 256) return false;
        const wchar_t* chars =
            reinterpret_cast<const wchar_t*>(reinterpret_cast<uint8_t*>(str) + 0x14);
        const int n =
            WideCharToMultiByte(CP_UTF8, 0, chars, len, out, outSz - 1, nullptr, nullptr);
        if (n <= 0) return false;
        out[n] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

void* TranslateCaption(void* text) {
    if (!il2::LooksLikeHeapPtr(text)) return text;
    char utf8[384]{};
    if (!ReadCaptionUtf8(text, utf8, static_cast<int>(sizeof(utf8)))) return text;
    const uint64_t hash = HashUtf8(utf8);
    if (void* hit = LookupZh(text, hash)) return hit;

    char work[384]{};
    std::memcpy(work, utf8, sizeof(work) - 1);
    const bool changed = zh::ApplyKoZh(work, sizeof(work));
    if (!changed || work[0] == 0 || std::strcmp(work, utf8) == 0) {
        if (zh::HasHangulUtf8(utf8) && gZhMissLogN < 64) {
            ++gZhMissLogN;
            x::runtime::LogI("UiCheatOverlay", "zh miss #%d [%s]", gZhMissLogN, utf8);
        }
        StoreZh(text, hash, text);
        return text;
    }
    if (!x::runtime::main_thread::IsOnPumpThread()) {
        x::runtime::LogWThrottled(133, 3000, "UiCheatOverlay",
                                  "zh skip off-pump [%s] — NewString 只许主泵", utf8);
        return text;
    }
    void* dst = il2::NewString(work);
    if (!il2::LooksLikeHeapPtr(dst)) return text;
    const auto& e = il2::Get();
    if (e.gcHandleNew) {
        __try {
            (void)e.gcHandleNew(dst, false);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    StoreZh(text, hash, dst);
    const bool leftover = zh::HasHangulUtf8(work);
    if (leftover && gZhMissLogN < 64) {
        ++gZhMissLogN;
        x::runtime::LogI("UiCheatOverlay", "zh leftover #%d [%s] → [%s]", gZhMissLogN, utf8, work);
    } else if (gZhCacheN <= 24 || leftover) {
        x::runtime::LogI("UiCheatOverlay", "zh [%s] → [%s] cache=%d", utf8, work, gZhCacheN);
    }
    return dst;
}

void* __fastcall HookTempRel5(void* text, void* a2) {
    const uint32_t n = gTempHits.fetch_add(1, std::memory_order_relaxed) + 1;
    char utf8[192]{};
    const bool got = ReadCaptionUtf8(text, utf8, static_cast<int>(sizeof(utf8)));
    if (n <= 12) {
        x::runtime::LogI("UiCheatOverlay", "zh Temp hit #%u hangul=%d [%s]", n,
                         (got && zh::HasHangulUtf8(utf8)) ? 1 : 0, got ? utf8 : "");
    } else {
        const DWORD now = GetTickCount();
        DWORD prev = gTempPulseMs.load(std::memory_order_relaxed);
        if (now - prev >= 2000 && gTempPulseMs.compare_exchange_strong(prev, now)) {
            x::runtime::LogI("UiCheatOverlay", "zh Temp pulse n=%u cache=%d", n, gZhCacheN);
        }
    }
    void* t = TranslateCaption(text);
    if (!gTempTramp) return nullptr;
    void* r = nullptr;
    __try {
        r = gTempTramp(t, a2);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLock("ui_cheat_overlay.TempRel5");
        r = nullptr;
    }
    return r;
}

void InstallZhHooks() {
    if (gZhInstalled.load(std::memory_order_acquire)) return;
    void* target = nullptr;
    if (il2::Ensure() && il2::GaBase()) target = il2::AtRva<void*>(kRvaGuiContentTemp);
    if (!target) {
        HMODULE ga = GetModuleHandleW(L"GameAssembly.dll");
        if (ga) target = reinterpret_cast<uint8_t*>(ga) + kRvaGuiContentTemp;
    }
    if (!target) {
        x::runtime::LogW("UiCheatOverlay", "zh Temp Rel5 miss: GameAssembly/RVA");
        return;
    }
    if (!InstallTempRel5(target, reinterpret_cast<void*>(&HookTempRel5))) {
        x::runtime::LogW("UiCheatOverlay", "zh Temp Rel5 install fail @%p rva=0x%X", target,
                         kRvaGuiContentTemp);
        return;
    }
    gZhInstalled.store(true, std::memory_order_release);
    x::runtime::LogI("UiCheatOverlay",
                     "zh Temp Rel5 installed steal=%zu target=%p tramp=%p near=%p", kTempSteal,
                     gTempAbs.target, gTempAbs.trampoline, gTempAbs.nearStub);
}

void RestoreZhHooks() {
    if (!gZhInstalled.exchange(false, std::memory_order_acq_rel)) return;
    RemoveTempAbs();
}

void* MiFn(void* method) {
    auto* mi = reinterpret_cast<MethodInfoHead*>(method);
    if (!mi) return nullptr;
    void* p = nullptr;
    __try {
        p = mi->methodPointer ? mi->methodPointer : mi->virtualMethodPointer;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        p = nullptr;
    }
    return p;
}

void* ResolveUiCheatKlass() {
    void* k = il2::FindClass("", kHashUiCheat);
    if (!k) k = il2::FindClass("", "UICheat");
    if (!k) k = il2::FindClass("Msc.UI", "UICheat");
    return k;
}

void* PickFindAll(void* klass) {
    if (!klass || !il2::Ensure()) return nullptr;
    const auto& e = il2::Get();
    if (!e.findAll) return nullptr;
    void* typeObj = il2::ClassTypeObjectOnMain(klass);
    if (!typeObj) return nullptr;
    void* arr = nullptr;
    __try {
        arr = e.findAll(typeObj, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLock("ui_cheat_overlay.FindAll");
        arr = nullptr;
    }
    const uintptr_t n = il2::ArrayLen(arr);
    for (uintptr_t i = 0; i < n && i < 16; ++i) {
        void* o = il2::ArrayAt(arr, i);
        if (il2::LooksLikeHeapPtr(o)) return o;
    }
    return nullptr;
}

void SetGoActive(void* go, bool on) {
    if (!il2::LooksLikeHeapPtr(go)) return;
    void* goKlass = il2::FindClass("UnityEngine", "GameObject");
    if (!goKlass) return;
    constexpr MethodShape kSet{1, TypeKind::Void, false, true, {TypeKind::Bool}};
    const auto mr = FindMethodResolved(goKlass, kRvaGoSetActive, kSet, "SetActive", nullptr);
    void* fn = MiFn(mr.method);
    if (!fn) fn = il2::AtRva<void*>(kRvaGoSetActive);
    if (!fn) return;
    using FnSet = void (*)(void* self, bool value, void* method);
    __try {
        reinterpret_cast<FnSet>(fn)(go, on, mr.method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLock("ui_cheat_overlay.SetActive");
    }
}

void SetBehaviourEnabled(void* inst, bool on) {
    if (!il2::LooksLikeHeapPtr(inst)) return;
    void* beh = il2::FindClass("UnityEngine", "Behaviour");
    if (!beh) return;
    constexpr MethodShape kSet{1, TypeKind::Void, false, true, {TypeKind::Bool}};
    const auto mr = FindMethodResolved(beh, 0, kSet, "set_enabled", nullptr);
    void* fn = MiFn(mr.method);
    if (!fn) return;
    using FnSet = void (*)(void* self, bool value, void* method);
    __try {
        reinterpret_cast<FnSet>(fn)(inst, on, mr.method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLock("ui_cheat_overlay.set_enabled");
    }
}

void* CompGameObject(void* inst) {
    if (!il2::LooksLikeHeapPtr(inst)) return nullptr;
    const auto& e = il2::Get();
    if (e.compGo) {
        void* go = nullptr;
        __try {
            go = e.compGo(inst, nullptr);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLock("ui_cheat_overlay.compGo");
            go = nullptr;
        }
        if (il2::LooksLikeHeapPtr(go)) return go;
    }
    void* compKlass = il2::FindClass("UnityEngine", "Component");
    if (!compKlass) return nullptr;
    constexpr MethodShape kGo{0, TypeKind::Ptr, true, true};
    const auto mr = FindMethodResolved(compKlass, kRvaCompGetGo, kGo, "get_gameObject", nullptr);
    void* fn = MiFn(mr.method);
    if (!fn) fn = il2::AtRva<void*>(kRvaCompGetGo);
    if (!fn) return nullptr;
    using FnGet = void* (*)(void* self, void* method);
    void* go = nullptr;
    __try {
        go = reinterpret_cast<FnGet>(fn)(inst, mr.method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLock("ui_cheat_overlay.get_gameObject");
        go = nullptr;
    }
    return il2::LooksLikeHeapPtr(go) ? go : nullptr;
}

void DontDestroy(void* obj) {
    if (!il2::LooksLikeHeapPtr(obj)) return;
    void* objKlass = il2::FindClass("UnityEngine", "Object");
    if (!objKlass) return;
    constexpr MethodShape kDd{1, TypeKind::Void, true, true, {TypeKind::Ptr}};
    const auto mr = FindMethodResolved(objKlass, kRvaDontDestroy, kDd, "DontDestroyOnLoad", nullptr);
    void* fn = MiFn(mr.method);
    if (!fn) fn = il2::AtRva<void*>(kRvaDontDestroy);
    if (!fn) return;
    using FnDd = void (*)(void* target, void* method);
    __try {
        reinterpret_cast<FnDd>(fn)(obj, mr.method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLock("ui_cheat_overlay.DontDestroyOnLoad");
    }
}

void* SpawnEmptyUiCheat(void* cheatKlass) {
    if (!cheatKlass) return nullptr;
    void* goKlass = il2::FindClass("UnityEngine", "GameObject");
    if (!goKlass) {
        x::runtime::LogW("UiCheatOverlay", "spawn skip: GameObject klass miss");
        return nullptr;
    }
    void* go = il2::AllocObject(goKlass);
    if (!il2::LooksLikeHeapPtr(go)) {
        x::runtime::LogW("UiCheatOverlay", "spawn skip: AllocObject GameObject failed");
        return nullptr;
    }
    void* name = il2::NewString("XCatUICheat");
    constexpr MethodShape kCtor{1, TypeKind::Void, false, true, {TypeKind::Ptr}};
    const auto ctorRes =
        FindMethodResolved(goKlass, kRvaGoCtorName, kCtor, nullptr, nullptr);
    void* ctorFn = MiFn(ctorRes.method);
    if (!ctorFn) ctorFn = il2::AtRva<void*>(kRvaGoCtorName);
    if (!ctorFn) {
        x::runtime::LogW("UiCheatOverlay", "spawn skip: GameObject.ctor(string) miss");
        return nullptr;
    }
    bool ctorOk = false;
    using FnCtor = void (*)(void* self, void* name, void* method);
    __try {
        reinterpret_cast<FnCtor>(ctorFn)(go, name, ctorRes.method);
        ctorOk = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ReturnLock("ui_cheat_overlay.GoCtor");
    }
    if (!ctorOk) {
        x::runtime::LogW("UiCheatOverlay", "spawn skip: GameObject.ctor except");
        return nullptr;
    }
    DontDestroy(go);
    SetGoActive(go, true);

    void* typeObj = il2::ClassTypeObjectOnMain(cheatKlass);
    if (!typeObj) {
        x::runtime::LogW("UiCheatOverlay", "spawn skip: UICheat Type miss");
        return nullptr;
    }
    constexpr MethodShape kAdd{1, TypeKind::Ptr, true, true, {TypeKind::Ptr}};
    const auto addRes =
        FindMethodResolved(goKlass, kRvaAddComponent, kAdd, "AddComponent", nullptr);
    void* addFn = MiFn(addRes.method);
    if (!addFn) addFn = il2::AtRva<void*>(kRvaAddComponent);
    if (!addFn) {
        x::runtime::LogW("UiCheatOverlay", "spawn skip: AddComponent miss");
        return nullptr;
    }
    using FnAdd = void* (*)(void* self, void* type, void* method);
    void* inst = nullptr;
    bool addExcept = false;
    __try {
        inst = reinterpret_cast<FnAdd>(addFn)(go, typeObj, addRes.method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        addExcept = true;
        ReturnLock("ui_cheat_overlay.AddComponent");
        inst = nullptr;
    }
    if (!il2::LooksLikeHeapPtr(inst)) {
        x::runtime::LogW("UiCheatOverlay", "spawn skip: AddComponent UICheat failed except=%d",
                         addExcept ? 1 : 0);
        return nullptr;
    }
    x::runtime::LogI("UiCheatOverlay", "spawn empty GO+AddComponent inst=%p go=%p", inst, go);
    return inst;
}

void CallOpen(void* klass, void* inst) {
    if (!klass || !il2::LooksLikeHeapPtr(inst)) return;
    constexpr MethodShape kOpen{0, TypeKind::Void, false, true};
    const auto mr = FindMethodResolved(klass, kRvaOpen, kOpen, "Open", kHashOpen);
    void* fn = MiFn(mr.method);
    if (!fn) fn = il2::AtRva<void*>(kRvaOpen);
    if (!fn) {
        x::runtime::LogW("UiCheatOverlay", "Open miss path=%s", PathName(mr.path));
        return;
    }
    bool except = false;
    using FnOpen = void (*)(void* self, void* method);
    __try {
        reinterpret_cast<FnOpen>(fn)(inst, mr.method);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        except = true;
        ReturnLock("ui_cheat_overlay.Open");
    }
    x::runtime::LogI("UiCheatOverlay", "Open path=%s except=%d", PathName(mr.path),
                     except ? 1 : 0);
}

void PumpJob(void* /*user*/) {
    InstallZhHooks();
    if (!world::IsPlayReady()) {
        x::runtime::LogW("UiCheatOverlay", "skip: not play-ready");
        return;
    }
    if (!il2::Ensure()) {
        x::runtime::LogW("UiCheatOverlay", "skip: il2cpp not ready");
        return;
    }

    void* klass = ResolveUiCheatKlass();
    if (!klass) {
        x::runtime::LogW("UiCheatOverlay", "skip: UICheat klass miss");
        return;
    }

    constexpr MethodShape kBool0{0, TypeKind::Bool, true, true};
    constexpr MethodShape kVoid0{0, TypeKind::Void, false, true};
    constexpr MethodShape kPtr0{0, TypeKind::Ptr, true, true};

    const auto isRes =
        FindMethodResolved(klass, kRvaIsInstantiated, kBool0, "IsInstantiated",
                           kHashIsInstantiated);
    const auto createRes =
        FindMethodResolved(klass, kRvaCreateInstance, kVoid0, "CreateInstance",
                           kHashCreateInstance);
    const auto getRes =
        FindMethodResolved(klass, kRvaGetInstance, kPtr0, "GetInstance", kHashGetInstance);

    void* isFn = MiFn(isRes.method);
    if (!isFn) isFn = il2::AtRva<void*>(kRvaIsInstantiated);
    void* createFn = MiFn(createRes.method);
    if (!createFn) createFn = il2::AtRva<void*>(kRvaCreateInstance);
    void* getFn = MiFn(getRes.method);
    if (!getFn) getFn = il2::AtRva<void*>(kRvaGetInstance);

    int already = -1;
    if (isFn) {
        using FnIs = bool (*)(void* method);
        __try {
            already = reinterpret_cast<FnIs>(isFn)(isRes.method) ? 1 : 0;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLock("ui_cheat_overlay.IsInstantiated");
            already = -2;
        }
    }

    int created = 0;
    int createExcept = 0;
    if (already != 1 && createFn) {
        using FnCreate = void (*)(void* method);
        __try {
            reinterpret_cast<FnCreate>(createFn)(createRes.method);
            created = 1;
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            createExcept = 1;
            ReturnLock("ui_cheat_overlay.CreateInstance");
        }
    }

    void* inst = nullptr;
    if (getFn) {
        using FnGet = void* (*)(void* method);
        __try {
            inst = reinterpret_cast<FnGet>(getFn)(getRes.method);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            ReturnLock("ui_cheat_overlay.GetInstance");
            inst = nullptr;
        }
    }
    const char* via = "GetInstance";
    if (!il2::LooksLikeHeapPtr(inst)) {
        inst = PickFindAll(klass);
        via = "FindAll";
    }
    int spawned = 0;
    if (!il2::LooksLikeHeapPtr(inst)) {
        inst = SpawnEmptyUiCheat(klass);
        via = "spawn";
        spawned = il2::LooksLikeHeapPtr(inst) ? 1 : 0;
    }

    x::runtime::LogI("UiCheatOverlay",
                     "klass=%p already=%d created=%d createEx=%d via=%s inst=%p "
                     "isPath=%s createPath=%s getPath=%s spawned=%d "
                     "(OnGUI not called here — Unity draws if GO active)",
                     klass, already, created, createExcept, via, inst,
                     PathName(isRes.path), PathName(createRes.path), PathName(getRes.path),
                     spawned);

    if (!il2::LooksLikeHeapPtr(inst)) {
        x::runtime::LogW("UiCheatOverlay",
                         "no UICheat instance — prefab/CreateInstance 可能被门挡");
        return;
    }

    CallOpen(klass, inst);
    void* go = CompGameObject(inst);
    SetGoActive(go, true);
    SetBehaviourEnabled(inst, true);
    x::runtime::LogI("UiCheatOverlay", "armed go=%p — look for IMGUI GM overlay in-game", go);
}

void PumpJobThunk(void* user) {
    PumpJob(user);
    gJobDone.store(true, std::memory_order_release);
}

DWORD WINAPI OpenThread(LPVOID) {
    if (gStop.load(std::memory_order_relaxed)) {
        gBusy.store(false, std::memory_order_release);
        return 0;
    }
    if (!x::runtime::main_thread::Ensure() || !x::runtime::main_thread::IsInstalled()) {
        x::runtime::LogW("UiCheatOverlay", "MainPump not installed");
        gBusy.store(false, std::memory_order_release);
        return 0;
    }
    gJobDone.store(false, std::memory_order_release);
    const bool ok = x::runtime::main_thread::InvokeAndWait(
        &PumpJobThunk, nullptr, kPumpTimeoutMs, x::runtime::main_thread::JobPrio::High);
    if (!ok) {
        x::runtime::LogW("UiCheatOverlay",
                         "InvokeAndWait failed/timeout %ums (换图 quiesce 或 CreateInstance 慢)",
                         static_cast<unsigned>(kPumpTimeoutMs));
        for (int i = 0; i < 100 && !gJobDone.load(std::memory_order_acquire); ++i) Sleep(20);
        gJobDone.store(true, std::memory_order_release);
    }
    gBusy.store(false, std::memory_order_release);
    return 0;
}

}  // namespace

void Init() {
    x::runtime::LogI("UiCheatOverlay",
                     "init — idle（不开 overlay 不 Rel5、不跑 OnGUI）；点按钮才 CreateInstance+Open，"
                     "并 Rel5 GUIContent.Temp；Shutdown 还原 .text。RVA Create=0x%X Open=0x%X Temp=0x%X",
                     kRvaCreateInstance, kRvaOpen, kRvaGuiContentTemp);
}

void Shutdown() {
    gStop.store(true, std::memory_order_release);
    for (int i = 0; i < 50 && gBusy.load(std::memory_order_acquire); ++i) Sleep(20);
    RestoreZhHooks();
}

void RequestOpen() {
    if (gStop.load(std::memory_order_relaxed)) return;
    bool expected = false;
    if (!gBusy.compare_exchange_strong(expected, true)) {
        x::runtime::LogW("UiCheatOverlay", "busy — refuse overlapping open");
        return;
    }
    HANDLE th = CreateThread(nullptr, 0, OpenThread, nullptr, 0, nullptr);
    if (!th) {
        gBusy.store(false, std::memory_order_release);
        x::runtime::LogW("UiCheatOverlay", "CreateThread failed");
        return;
    }
    CloseHandle(th);
}

}  // namespace x::features::ui_cheat_overlay
